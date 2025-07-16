#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pipe_fs_i.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

#define FOO_PATH "/sys/devices/virtual/oplus_chg/common"
#define BATT_PATH "/sys/devices/virtual/oplus_chg/battery"
#define BCC BATT_PATH "/bcc_current"
#define NCD BATT_PATH "/normal_cool_down"
#define CD BATT_PATH "/cool_down"
#define CURRENT_NOW "/sys/devices/platform/soc/soc:oplus,mms_gauge/oplus_mms/gauge/battery/current_now"
#define BCC_CUR "9100"

// 定义延迟工作结构
static struct delayed_work charging_work;

// 执行用户空间命令并通过管道获取输出
/*
 * 注意：此函数仅为测试目的而设计，存在安全和稳定性风险。
 * 它通过 usermode-helper API 重定向输出来捕获脚本执行结果。
 */
static int exec_user_command(const char *script, char *output, size_t output_size) {
    struct subprocess_info *info;
    struct file *pipe_read, *pipe_write;
    int fds[2];
    int ret;
    // 为命令准备参数列表和环境
    const char *cmd_path = "/bin/sh"; // 通常在/bin/sh，而非/system/bin/sh
    char *cmd_argv[] = {(char *)cmd_path, "-c", (char *)script, NULL};
    char *cmd_envp[] = {"HOME=/", "PATH=/sbin:/vendor/bin:/system/bin:/bin", NULL};

    // 1. 创建管道用于捕获输出
    // 移除了 O_NONBLOCK，因为我们会在子进程结束后再读取，此时读取不会无限阻塞
    ret = do_pipe_flags(fds, 0);
    if (ret < 0) {
        pr_err("unrestricted_charging: 创建管道失败: %d\n", ret);
        return ret;
    }
    pipe_read = fget(fds[0]);
    pipe_write = fget(fds[1]);

    // 2. 设置 usermode-helper
    // call_usermodehelper_setup 分配和准备 subprocess_info 结构
    info = call_usermodehelper_setup((char *)cmd_path, cmd_argv, cmd_envp, GFP_KERNEL);
    if (!info) {
        pr_err("unrestricted_charging: 设置 usermode helper 失败\n");
        // 清理管道
        fput(pipe_read);
        fput(pipe_write);
        return -ENOMEM;
    }

    // 3. 【核心】将子进程的标准输出重定向到管道的写入端
    // call_usermodehelper_exec 会增加 pipe_write 的引用计数
    info->stdout = pipe_write;

    // 4. 执行命令并等待其完成
    ret = call_usermodehelper_exec(info, UMH_WAIT_PROC);
    // info 结构在 exec 后已被释放，无需我们手动处理

    // 5. 从管道读取命令的输出
    // 此时子进程已退出，管道的写入端已关闭，所以读取操作会正确地在数据末尾结束
    if (output && output_size > 0 && ret == 0) {
        loff_t pos = 0;
        ssize_t bytes_read;
        
        // 清空输出缓冲区
        memset(output, 0, output_size);

        bytes_read = kernel_read(pipe_read, output, output_size - 1, &pos);
        if (bytes_read >= 0) {
            // kernel_read 不会添加空终止符，我们手动添加
            output[bytes_read] = '\0'; 
            pr_info("unrestricted_charging: 命令输出 (%zd bytes): %s\n", bytes_read, output);
        } else {
            pr_err("unrestricted_charging: 读取管道输出失败: %zd\n", bytes_read);
        }
    } else if (ret != 0) {
        pr_err("unrestricted_charging: 执行命令 '%s' 失败: %d\n", script, ret);
    }

    // 6. 清理管道文件描述符
    // 即使我们将 pipe_write 传给了 helper，我们仍然需要在这里释放我们自己的引用
    fput(pipe_read);
    fput(pipe_write);

    return ret;
}

// 执行充电控制逻辑
static void charging_control(void) {
    char command[512];
    char output[256];

    printk(KERN_INFO "unrestricted_charging: === 开始执行充电控制 ===\n");

    // 检查必要节点是否存在
    snprintf(command, sizeof(command), 
             "[ -f " FOO_PATH "/deep_dischg_counts ] && [ -f " BCC " ] && [ -f " NCD " ] && [ -f " CD " ]");
    if (exec_user_command(command, output, sizeof(output)) != 0) {
        printk(KERN_ERR "unrestricted_charging: 必要 sysfs 节点尚未准备好，稍后重试\n");
        schedule_delayed_work(&charging_work, msecs_to_jiffies(1000)); // 延迟 1000ms 后重试
        return;
    }

    // 保存原始 deep_dischg_counts
    snprintf(command, sizeof(command), "cat " FOO_PATH "/deep_dischg_counts > /data/local/tmp/origin_ddc");
    exec_user_command(command, output, sizeof(output));
    printk(KERN_INFO "unrestricted_charging: 原始 deep_dischg_counts: %s\n", output);

    // 触发标志位设置
    snprintf(command, sizeof(command), "echo 1 > " FOO_PATH "/deep_dischg_counts");
    exec_user_command(command, output, sizeof(output));
    snprintf(command, sizeof(command), "echo 1 > " FOO_PATH "/deep_dischg_count_cali");
    exec_user_command(command, output, sizeof(output));

    // 还原初始值
    snprintf(command, sizeof(command), "cat /data/local/tmp/origin_ddc > " FOO_PATH "/deep_dischg_counts");
    exec_user_command(command, output, sizeof(output));
    snprintf(command, sizeof(command), "echo 0 > " FOO_PATH "/deep_dischg_count_cali");
    exec_user_command(command, output, sizeof(output));

    // 设置权限
    snprintf(command, sizeof(command), "chmod 644 " BCC);
    exec_user_command(command, output, sizeof(output));
    snprintf(command, sizeof(command), "chmod 644 " NCD);
    exec_user_command(command, output, sizeof(output));
    snprintf(command, sizeof(command), "chmod 644 " CD);
    exec_user_command(command, output, sizeof(output));

    // 重设 cooldown 状态
    snprintf(command, sizeof(command), "echo 0 > " CD);
    exec_user_command(command, output, sizeof(output));
    snprintf(command, sizeof(command), "echo 0 > " NCD);
    exec_user_command(command, output, sizeof(output));

    // 设置 bcc 电流
    snprintf(command, sizeof(command), "echo " BCC_CUR " > " BCC);
    exec_user_command(command, output, sizeof(output));

    // 恢复权限为只读
    snprintf(command, sizeof(command), "chmod 400 " BCC);
    exec_user_command(command, output, sizeof(output));
    snprintf(command, sizeof(command), "chmod 400 " NCD);
    exec_user_command(command, output, sizeof(output));
    snprintf(command, sizeof(command), "chmod 400 " CD);
    exec_user_command(command, output, sizeof(output));

    // HIDL 电流投票读取权限恢复
    snprintf(command, sizeof(command), "chmod 444 " CURRENT_NOW);
    exec_user_command(command, output, sizeof(output));

    // 输出当前值
    snprintf(command, sizeof(command), "cat " FOO_PATH "/deep_dischg_counts");
    exec_user_command(command, output, sizeof(output));
    printk(KERN_INFO "unrestricted_charging: 当前 deep_dischg_counts: %s\n", output);
    snprintf(command, sizeof(command), "cat " BCC);
    exec_user_command(command, output, sizeof(output));
    printk(KERN_INFO "unrestricted_charging: 当前 bcc_current: %s\n", output);
    snprintf(command, sizeof(command), "cat " NCD);
    exec_user_command(command, output, sizeof(output));
    printk(KERN_INFO "unrestricted_charging: 当前 normal_cool_down: %s\n", output);
    snprintf(command, sizeof(command), "cat " CD);
    exec_user_command(command, output, sizeof(output));
    printk(KERN_INFO "unrestricted_charging: 当前 cool_down: %s\n", output);
    snprintf(command, sizeof(command), "cat " CURRENT_NOW);
    exec_user_command(command, output, sizeof(output));
    printk(KERN_INFO "unrestricted_charging: 当前 current_now: %s\n", output);

    printk(KERN_INFO "unrestricted_charging: === 充电控制执行完成 ===\n");
}

// 工作队列处理函数
static void charging_work_func(struct work_struct *work) {
    charging_control();
    // 每隔5秒重新调度工作
    schedule_delayed_work(&charging_work, msecs_to_jiffies(5000));
}

// 模块初始化
static int __init my_module_init(void) {
    printk(KERN_INFO "unrestricted_charging: Initializing sysfs control module\n");
    printk(KERN_INFO "unrestricted_charging: Author: CoolAPK@BrokeStar, modified from CoolAPK@Bybycode's unrestricted charging script\n");

    // 初始化延迟工作
    INIT_DELAYED_WORK(&charging_work, charging_work_func);
    schedule_delayed_work(&charging_work, msecs_to_jiffies(5000)); // 首次延迟 5 秒后执行

    return 0;
}

// 模块退出
static void __exit my_module_exit(void) {
    cancel_delayed_work_sync(&charging_work); // 取消未完成的工作
    printk(KERN_INFO "unrestricted_charging: Exiting sysfs control module\n");
}

module_init(my_module_init);
module_exit(my_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CoolAPK@BrokeStar");
MODULE_AUTHOR("CoolAPK@Bybycode");
MODULE_DESCRIPTION("The kernel module achieves unrestricted charging by calling user-space commands to modify sysfs nodes.");

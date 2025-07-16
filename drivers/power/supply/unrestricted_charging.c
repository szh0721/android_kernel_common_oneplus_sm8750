#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/init.h>
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

// 辅助函数：执行用户空间命令
static int exec_user_command(const char *script) {
    char *cmd_path = "/bin/sh";
    char *cmd_argv[] = {cmd_path, "-c", (char *)script, NULL};
    char *cmd_envp[] = {"HOME=/", "PATH=/sbin:/bin:/usr/bin", NULL};
    int ret;

    ret = call_usermodehelper(cmd_path, cmd_argv, cmd_envp, UMH_WAIT_PROC);
    if (ret != 0) {
        printk(KERN_ERR "unrestricted_charging: Failed to execute command '%s': %d\n", script, ret);
    } else {
        printk(KERN_INFO "unrestricted_charging: Command '%s' executed successfully\n", script);
    }
    return ret;
}

// 执行充电控制逻辑
static void charging_control(void) {
    char command[512];

    printk(KERN_INFO "unrestricted_charging: === 开始执行充电控制 ===\n");

    // 检查必要节点是否存在
    snprintf(command, sizeof(command), 
             "[ -f " FOO_PATH "/deep_dischg_counts ] && [ -f " BCC " ] && [ -f " NCD " ] && [ -f " CD " ]");
    if (exec_user_command(command) != 0) {
        printk(KERN_ERR "unrestricted_charging: 必要 sysfs 节点尚未准备好，稍后重试\n");
        schedule_delayed_work(&charging_work, msecs_to_jiffies(1000)); // 延迟 1000ms 后重试
        return;
    }

    // 保存原始 deep_dischg_counts
    snprintf(command, sizeof(command), "cat " FOO_PATH "/deep_dischg_counts > /tmp/origin_ddc");
    exec_user_command(command);

    // 触发标志位设置
    snprintf(command, sizeof(command), "echo 1 > " FOO_PATH "/deep_dischg_counts");
    exec_user_command(command);
    snprintf(command, sizeof(command), "echo 1 > " FOO_PATH "/deep_dischg_count_cali");
    exec_user_command(command);

    // 还原初始值
    snprintf(command, sizeof(command), "cat /tmp/origin_ddc > " FOO_PATH "/deep_dischg_counts");
    exec_user_command(command);
    snprintf(command, sizeof(command), "echo 0 > " FOO_PATH "/deep_dischg_count_cali");
    exec_user_command(command);

    // 设置权限
    snprintf(command, sizeof(command), "chmod 644 " BCC);
    exec_user_command(command);
    snprintf(command, sizeof(command), "chmod 644 " NCD);
    exec_user_command(command);
    snprintf(command, sizeof(command), "chmod 644 " CD);
    exec_user_command(command);

    // 重设 cooldown 状态
    snprintf(command, sizeof(command), "echo 0 > " CD);
    exec_user_command(command);
    snprintf(command, sizeof(command), "echo 0 > " NCD);
    exec_user_command(command);

    // 设置 bcc 电流
    snprintf(command, sizeof(command), "echo " BCC_CUR " > " BCC);
    exec_user_command(command);

    // 恢复权限为只读
    snprintf(command, sizeof(command), "chmod 400 " BCC);
    exec_user_command(command);
    snprintf(command, sizeof(command), "chmod 400 " NCD);
    exec_user_command(command);
    snprintf(command, sizeof(command), "chmod 400 " CD);
    exec_user_command(command);

    // HIDL 电流投票读取权限恢复
    snprintf(command, sizeof(command), "chmod 444 " CURRENT_NOW);
    exec_user_command(command);

    // 输出当前值
    snprintf(command, sizeof(command), "cat " FOO_PATH "/deep_dischg_counts");
    exec_user_command(command);
    snprintf(command, sizeof(command), "cat " BCC);
    exec_user_command(command);
    snprintf(command, sizeof(command), "cat " NCD);
    exec_user_command(command);
    snprintf(command, sizeof(command), "cat " CD);
    exec_user_command(command);
    snprintf(command, sizeof(command), "cat " CURRENT_NOW);
    exec_user_command(command);

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

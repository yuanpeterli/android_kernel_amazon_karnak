#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/acpi.h>
#include <linux/thermal.h>
#include <linux/platform_device.h>
#include <mt-plat/aee.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
/*#include "mach/dma.h"*/
#include <mt-plat/sync_write.h>
/*/#include <mach/mt_irq.h>*/
#include "mt-plat/mtk_thermal_monitor.h"
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include "mach/mt_typedefs.h"
#include "mach/mt_thermal.h"
/* #include <mach/mt_ptp.h> */
/* #include "mach/mt_gpufreq.h" */
/* #include <mach/mt_wtd.h> */
#include <mach/wd_api.h>
#include <linux/time.h>
#include <linux/uidgid.h>
#include <linux/platform_data/mtk_thermal.h>

#include "inc/mtk_ts_cpu.h"


#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#endif
#define __MT_MTK_TS_CPU_C__


#ifdef CONFIG_OF

u32 thermal_irq_number = 0;
void __iomem *thermal_base;
void __iomem *auxadc_ts_base;
void __iomem *apmixed_base;
void __iomem *INFRACFG_AO_BASE;

int thermal_phy_base;
int auxadc_ts_phy_base;
int apmixed_phy_base;
int pericfg_phy_base;

struct clk *clk_peri_therm;
struct clk *clk_auxadc;
#endif


/* Workaround, it will remove after PTP driver ready. */
#if 1
/*
 * lock
 */
static DEFINE_SPINLOCK(ptp_spinlock);

void mt_ptp_lock(unsigned long *x)
{
	spin_lock_irqsave(&ptp_spinlock, *x);
	/*return 0;*/
};
EXPORT_SYMBOL(mt_ptp_lock);

void mt_ptp_unlock(unsigned long *x)
{
	spin_unlock_irqrestore(&ptp_spinlock, *x);
	/*return 0;*/
};
EXPORT_SYMBOL(mt_ptp_unlock);
#endif


/*==============*/
/*Configurations*/
/*==============*/
/* 1: turn on RT kthread for thermal protection in this sw module; 0: turn off */
#define MTK_TS_CPU_RT                       (1)
#if MTK_TS_CPU_RT
#include <linux/sched.h>
#include <linux/kthread.h>
#endif


/*=============================================================
 * Macro definition
 *=============================================================*/

/*
 * CONFIG (SW related)
 */
#define THERMAL_TURNOFF_AUXADC_BEFORE_DEEPIDLE    (1)


#define THERMAL_PERFORMANCE_PROFILE         (0)

/* 1: turn on GPIO toggle monitor; 0: turn off */
#define THERMAL_GPIO_OUT_TOGGLE             (1)



/* 1: turn on adaptive AP cooler; 0: turn off */
#define CPT_ADAPTIVE_AP_COOLER              (1)

/* 1: turn on supports to MET logging; 0: turn off */
#define CONFIG_SUPPORT_MET_MTKTSCPU         (0)

/* Thermal controller HW filtering function.
		Only 1, 2, 4, 8, 16 are valid values, they means one reading is a avg of X samples */
#define THERMAL_CONTROLLER_HW_FILTER        (1)	/* 1, 2, 4, 8, 16 */

/* 1: turn on thermal controller HW thermal protection; 0: turn off */
#define THERMAL_CONTROLLER_HW_TP            (1)

/* 1: turn on SW filtering in this sw module; 0: turn off */
#define MTK_TS_CPU_SW_FILTER                (1)



/* 1: turn on fast polling in this sw module; 0: turn off */
#define MTKTSCPU_FAST_POLLING               (1)

#if CPT_ADAPTIVE_AP_COOLER
#define MAX_CPT_ADAPTIVE_COOLERS            (3)

#define THERMAL_HEADROOM                    (1)
#endif

struct proc_dir_entry *mtk_thermal_get_proc_drv_therm_dir_entry(void);

static void tscpu_fast_initial_sw_workaround(void);



/* 1: thermal driver fast polling, use hrtimer; 0: turn off */
/* #define THERMAL_DRV_FAST_POLL_HRTIMER          (1) */

/* 1: thermal driver update temp to MET directly, use hrtimer; 0: turn off */
#define THERMAL_DRV_UPDATE_TEMP_DIRECT_TO_MET  (1)


#define MIN(_a_, _b_) ((_a_) > (_b_) ? (_b_) : (_a_))
#define MAX(_a_, _b_) ((_a_) > (_b_) ? (_a_) : (_b_))

/*=============================================================
 *REG ACCESS
 *=============================================================*/

#define thermal_readl(addr)         DRV_Reg32(addr)
#define thermal_writel(addr, val)   mt_reg_sync_writel((val), ((void *)addr))
#define thermal_setl(addr, val)     mt_reg_sync_writel(thermal_readl(addr) | (val), ((void *)addr))
#define thermal_clrl(addr, val)     mt_reg_sync_writel(thermal_readl(addr) & ~(val), ((void *)addr))


/*=============================================================
 *Local variable definition
 *=============================================================*/

#if CPT_ADAPTIVE_AP_COOLER
static int g_curr_temp;
static int g_prev_temp;
#endif

static kuid_t uid = KUIDT_INIT(0);
static kgid_t gid = KGIDT_INIT(1000);

#if MTKTSCPU_FAST_POLLING
/* Combined fast_polling_trip_temp and fast_polling_factor,
it means polling_delay will be 1/5 of original interval after
mtktscpu reports > 65C w/o exit point */
static int fast_polling_trip_temp = 60000;
static int fast_polling_factor = 5;
static int cur_fp_factor = 1;
static int next_fp_factor = 1;
#endif


static int g_max_temp = 50000;	/* default=50 deg */
static int g_temp = 50000;


static unsigned int interval = 1000;	/* mseconds, 0 : no auto polling */
/* trip_temp[0] must be initialized to the thermal HW protection point. */
static int trip_temp[10] = {
	117000, 100000, 85000, 75000, 65000, 55000, 45000, 35000, 25000, 15000
};



static unsigned int *cl_dev_state;
static unsigned int cl_dev_sysrst_state;
#if CPT_ADAPTIVE_AP_COOLER
static unsigned int cl_dev_adp_cpu_state[MAX_CPT_ADAPTIVE_COOLERS] = { 0 };

static unsigned int cl_dev_adp_cpu_state_active;
static int active_adp_cooler;
#endif
static struct thermal_zone_device *thz_dev;

static struct thermal_cooling_device **cl_dev;
static struct thermal_cooling_device *cl_dev_sysrst;
#if CPT_ADAPTIVE_AP_COOLER
static struct thermal_cooling_device *cl_dev_adp_cpu[MAX_CPT_ADAPTIVE_COOLERS] = { NULL };
#endif

#if CPT_ADAPTIVE_AP_COOLER
static int TARGET_TJS[MAX_CPT_ADAPTIVE_COOLERS] = { 85000, 0 };
static int PACKAGE_THETA_JA_RISES[MAX_CPT_ADAPTIVE_COOLERS] = { 35, 0 };
static int PACKAGE_THETA_JA_FALLS[MAX_CPT_ADAPTIVE_COOLERS] = { 25, 0 };
static int MINIMUM_CPU_POWERS[MAX_CPT_ADAPTIVE_COOLERS] = { 1200, 0 };
static int MAXIMUM_CPU_POWERS[MAX_CPT_ADAPTIVE_COOLERS] = { 4400, 0 };
static int MINIMUM_GPU_POWERS[MAX_CPT_ADAPTIVE_COOLERS] = { 350, 0 };
static int MAXIMUM_GPU_POWERS[MAX_CPT_ADAPTIVE_COOLERS] = { 960, 0 };
static int FIRST_STEP_TOTAL_POWER_BUDGETS[MAX_CPT_ADAPTIVE_COOLERS] = { 3300, 0 };
static int MINIMUM_BUDGET_CHANGES[MAX_CPT_ADAPTIVE_COOLERS] = { 50, 0 };

#if THERMAL_HEADROOM
static int p_Tpcb_correlation;
static int Tpcb_trip_point;

static int thp_max_cpu_power;
static int thp_p_tj_correlation;
static int thp_threshold_tj;
#endif

#endif



static U32 calefuse1;
static U32 calefuse2;
static U32 calefuse3;

/* +ASC+ */
#if CPT_ADAPTIVE_AP_COOLER
/*0: default:  ATM v1 */
/*1:	 FTL ATM v2 */
/*2:	 CPU_GPU_Weight ATM v2 */
static int mtktscpu_atm = 1;
static int tt_ratio_high_rise = 1;
static int tt_ratio_high_fall = 1;
static int tt_ratio_low_rise = 1;
static int tt_ratio_low_fall = 1;
static int tp_ratio_high_rise = 1;
static int tp_ratio_high_fall;
static int tp_ratio_low_rise;
static int tp_ratio_low_fall;
/* static int cpu_loading = 0; */
#endif
/* -ASC- */

static int mtktscpu_debug_log;
static int kernelmode;
static int g_THERMAL_TRIP[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static int num_trip = 5;

int MA_len_temp = 0;
static int proc_write_flag;
static char *cooler_name;
#define CPU_COOLER_NUM 34

static DEFINE_MUTEX(TS_lock);


#if CPT_ADAPTIVE_AP_COOLER
static const char adaptive_cooler_name[] = "cpu_adaptive_";
#endif

static char g_bind0[20] = "mtktscpu-sysrst";
static char g_bind1[20] = "cpu02";
static char g_bind2[20] = "cpu15";
static char g_bind3[20] = "cpu22";
static char g_bind4[20] = "cpu28";
static char g_bind5[20] = "";
static char g_bind6[20] = "";
static char g_bind7[20] = "";
static char g_bind8[20] = "";
static char g_bind9[20] = "";

static int read_curr_temp;

/**
 * If curr_temp >= polling_trip_temp1, use interval
 * else if cur_temp >= polling_trip_temp2 && curr_temp < polling_trip_temp1, use interval*polling_factor1
 * else, use interval*polling_factor2
 */
static int polling_trip_temp1 = 40000;
static int polling_trip_temp2 = 20000;
static int polling_factor1 = 2;
static int polling_factor2 = 4;

#define MTKTSCPU_TEMP_CRIT 120000	/* 120.000 degree Celsius */

#if MTK_TS_CPU_RT
static struct task_struct *ktp_thread_handle;
#endif

static int tc_mid_trip = -275000;

/*
Bank0 : CPU (TS_MCU1)        (TS1)
Bank1 : GPU (TS_MCU2)                (TS2)
Bank2 : SOC (TS_MCU3)(TS13)

TS_MCU1: TS3 (9464.54, 65.8)
TS_MCU2: TS4 (6745.06, 2790.2)
TS_MCU3: TS5 (5673.50, 6392.4)
TS_MCU4: TS1 (3349.22, 4163.6)
TS_ABB:  TS2
 */

static int CPU_TS_MCU1_T = 0, CPU_TS_MCU2_T;
static int GPU_TS_MCU1_T = 0, GPU_TS_MCU2_T;
static int SOC_TS_MCU1_T = 0, SOC_TS_MCU2_T = 0, SOC_TS_MCU3_T;
#if 1
static int CPU_TS_MCU1_R = 0, CPU_TS_MCU2_R;
static int GPU_TS_MCU1_R = 0, GPU_TS_MCU2_R;
static int SOC_TS_MCU1_R = 0, SOC_TS_MCU2_R = 0, SOC_TS_MCU3_R;
#endif

int last_abb_t = 0;
int last_CPU1_t = 0;
int last_CPU2_t = 0;

static int g_tc_resume;		/* default=0,read temp */

static S32 g_adc_ge_t;
static S32 g_adc_oe_t;
static S32 g_o_vtsmcu1;
static S32 g_o_vtsmcu2;
static S32 g_o_vtsmcu3;
static S32 g_o_vtsmcu4;
static S32 g_o_vtsabb;
static S32 g_degc_cali;
static S32 g_adc_cali_en_t;
static S32 g_o_slope;
static S32 g_o_slope_sign;
static S32 g_id;

static S32 g_ge = 1;
static S32 g_oe = 1;
static S32 g_gain = 1;

static S32 g_x_roomt[THERMAL_SENSOR_NUM] = { 0 };

static int Num_of_OPP;

#if 0
static int Num_of_GPU_OPP = 1;	/* Set this value =1 for non-DVS GPU */
#else				/* DVFS GPU */
static int Num_of_GPU_OPP;
#endif


#define y_curr_repeat_times 1
#define THERMAL_NAME    "mt8163-thermal"
/* #define GPU_Default_POWER     456 */

static struct mtk_cpu_power_info *mtk_cpu_power;
static int tscpu_num_opp;
static struct mtk_gpu_power_info *mtk_gpu_power;

static int tscpu_cpu_dmips[CPU_COOLER_NUM] = { 0 };

int mtktscpu_limited_dmips = 0;

static bool talking_flag;
static int temperature_switch;


#define TS_MS_TO_NS(x) (x * 1000 * 1000)
static struct hrtimer ts_tempinfo_hrtimer;


/*=============================================================
 *LOG
 *=============================================================*/

#define tscpu_dprintk(fmt, args...)   \
	do {                                    \
		if (mtktscpu_debug_log) {                \
			pr_debug("Power/CPU_Thermal" fmt, ##args); \
		}                                   \
	} while (0)

#define tscpu_printk(fmt, args...) pr_debug("Power/CPU_Thermal" fmt, ##args)


/*=============================================================
 * Local function declartation
 *=============================================================*/

static int mtktscpu_switch_bank(enum thermal_bank_name bank);
static void tscpu_reset_thermal(void);
static S32 temperature_to_raw_room(U32 ret);
static void set_tc_trigger_hw_protect(int temperature, int temperature2);
static void tscpu_config_all_tc_hw_protect(int temperature, int temperature2);
static void thermal_initial_all_bank(void);
static void read_each_bank_TS(enum thermal_bank_name bank_num);

static int tscpu_register_thermal(void);
static void tscpu_unregister_thermal(void);
static void tscpu_set_GPIO_toggle_for_monitor(void);
static int thermal_fast_init(void);

static void tscpu_update_temperature_timer_init(void);

	bool __attribute__ ((weak))
mtk_get_gpu_loading(unsigned int *pLoading)
{
	return 0;
}

	void __attribute__ ((weak))
mt_cpufreq_thermal_protect(unsigned int limited_power)
{
}

	void __attribute__ ((weak))
mt_gpufreq_thermal_protect(unsigned int limited_power)
{
}


	unsigned int __attribute__ ((weak))
mt_gpufreq_get_cur_freq(void)
{
	return 0;
}


/*=============================================================
 * Local type definition
 *=============================================================*/


struct mtk_cpu_power_info {
	unsigned int cpufreq_khz;
	unsigned int cpufreq_ncpu;
	unsigned int cpufreq_power;
};
struct mtk_gpu_power_info {
	unsigned int gpufreq_khz;
	unsigned int gpufreq_power;
};



/*=============================================================
 * Local function definition
 *=============================================================*/


#if THERMAL_DRV_UPDATE_TEMP_DIRECT_TO_MET
static int a_tscpu_all_temp[THERMAL_SENSOR_NUM] = { 0 };

static DEFINE_MUTEX(MET_GET_TEMP_LOCK);
static met_thermalsampler_funcMET g_pThermalSampler;
void mt_thermalsampler_registerCB(met_thermalsampler_funcMET pCB)
{
	g_pThermalSampler = pCB;
}
EXPORT_SYMBOL(mt_thermalsampler_registerCB);

static DEFINE_SPINLOCK(tscpu_met_spinlock);
void tscpu_met_lock(unsigned long *flags)
{
	spin_lock_irqsave(&tscpu_met_spinlock, *flags);
}
EXPORT_SYMBOL(tscpu_met_lock);

void tscpu_met_unlock(unsigned long *flags)
{
	spin_unlock_irqrestore(&tscpu_met_spinlock, *flags);
}
EXPORT_SYMBOL(tscpu_met_unlock);

#endif


void set_taklking_flag(bool flag)
{
	talking_flag = flag;
	tscpu_printk("talking_flag=%d\n", talking_flag);
}

static unsigned int adaptive_cpu_power_limit = 0x7FFFFFFF, static_cpu_power_limit = 0x7FFFFFFF;
static unsigned int prv_adp_cpu_pwr_lim, prv_stc_cpu_pwr_lim;


static void set_adaptive_cpu_power_limit(unsigned int limit)
{
	unsigned int final_limit;

	prv_adp_cpu_pwr_lim = adaptive_cpu_power_limit;
	adaptive_cpu_power_limit = (limit != 0) ? limit : 0x7FFFFFFF;
	final_limit = MIN(adaptive_cpu_power_limit, static_cpu_power_limit);

	if (prv_adp_cpu_pwr_lim != adaptive_cpu_power_limit) {
		tscpu_printk("set_adaptive_cpu_power_limit %d, T=%d,%d,%d,%d,%d,%d\n",
				(final_limit != 0x7FFFFFFF) ? final_limit : 0, CPU_TS_MCU1_T,
				CPU_TS_MCU2_T, GPU_TS_MCU2_T, SOC_TS_MCU1_T, SOC_TS_MCU2_T,
				SOC_TS_MCU3_T);

		thermal_budget_notify((final_limit != 0x7FFFFFFF) ? final_limit : 0);
	}
}

static void set_static_cpu_power_limit(unsigned int limit)
{
	unsigned int final_limit;

	prv_stc_cpu_pwr_lim = static_cpu_power_limit;
	static_cpu_power_limit = (limit != 0) ? limit : 0x7FFFFFFF;
	final_limit = MIN(adaptive_cpu_power_limit, static_cpu_power_limit);

	if (prv_stc_cpu_pwr_lim != static_cpu_power_limit) {
		tscpu_printk("set_static_cpu_power_limit %d, T=%d,%d,%d,%d,%d,%d\n",
				(final_limit != 0x7FFFFFFF) ? final_limit : 0, CPU_TS_MCU1_T,
				CPU_TS_MCU2_T, GPU_TS_MCU2_T, SOC_TS_MCU1_T, SOC_TS_MCU2_T,
				SOC_TS_MCU3_T);

		thermal_budget_notify((final_limit != 0x7FFFFFFF) ? final_limit : 0);
	}
}


static unsigned int adaptive_gpu_power_limit = 0x7FFFFFFF, static_gpu_power_limit = 0x7FFFFFFF;


static void set_adaptive_gpu_power_limit(unsigned int limit)
{
	unsigned int final_limit;

	adaptive_gpu_power_limit = (limit != 0) ? limit : 0x7FFFFFFF;
	final_limit = MIN(adaptive_gpu_power_limit, static_gpu_power_limit);
	tscpu_printk("set_adaptive_gpu_power_limit %d\n",
			(final_limit != 0x7FFFFFFF) ? final_limit : 0);
	mt_gpufreq_thermal_protect((final_limit != 0x7FFFFFFF) ? final_limit : 0);
}

static void set_static_gpu_power_limit(unsigned int limit)
{
	unsigned int final_limit;

	static_gpu_power_limit = (limit != 0) ? limit : 0x7FFFFFFF;
	final_limit = MIN(adaptive_gpu_power_limit, static_gpu_power_limit);
	tscpu_printk("set_static_gpu_power_limit %d\n",
			(final_limit != 0x7FFFFFFF) ? final_limit : 0);
	mt_gpufreq_thermal_protect((final_limit != 0x7FFFFFFF) ? final_limit : 0);
}


/* TODO: We also need a pair of setting functions for GPU power limit, which is not supported on MT6582. */
#if 0
int tscpu_thermal_clock_on(void)
{
	int ret = -1;

	tscpu_printk("tscpu_thermal_clock_on\n");

	ret = enable_clock(MT_CG_INFRA_THERM, "THERMAL");

	return ret;
}

int tscpu_thermal_clock_off(void)
{
	int ret = -1;

	tscpu_dprintk("tscpu_thermal_clock_off\n");

	ret = disable_clock(MT_CG_INFRA_THERM, "THERMAL");

	return ret;
}
#else
void tscpu_thermal_clock_on(void)
{
	pr_debug("tscpu_thermal_clock_on\n");
	clk_prepare(clk_auxadc);
	clk_enable(clk_auxadc);
	clk_prepare(clk_peri_therm);
	clk_enable(clk_peri_therm);
}

void tscpu_thermal_clock_off(void)
{
	pr_debug("tscpu_thermal_clock_off\n");
	clk_disable(clk_peri_therm);
	clk_unprepare(clk_peri_therm);
	clk_disable(clk_auxadc);
	clk_unprepare(clk_auxadc);
}

#endif



void get_thermal_all_register(void)
{
	tscpu_dprintk("get_thermal_all_register\n");

	tscpu_dprintk("TEMPMSR1		  = 0x%8x\n", DRV_Reg32(TEMPMSR1));
	tscpu_dprintk("TEMPMSR2            = 0x%8x\n", DRV_Reg32(TEMPMSR2));

	tscpu_dprintk("TEMPMONCTL0	  = 0x%8x\n", DRV_Reg32(TEMPMONCTL0));
	tscpu_dprintk("TEMPMONCTL1	  = 0x%8x\n", DRV_Reg32(TEMPMONCTL1));
	tscpu_dprintk("TEMPMONCTL2	  = 0x%8x\n", DRV_Reg32(TEMPMONCTL2));
	tscpu_dprintk("TEMPMONINT	  = 0x%8x\n", DRV_Reg32(TEMPMONINT));
	tscpu_dprintk("TEMPMONINTSTS	  = 0x%8x\n", DRV_Reg32(TEMPMONINTSTS));
	tscpu_dprintk("TEMPMONIDET0	  = 0x%8x\n", DRV_Reg32(TEMPMONIDET0));

	tscpu_dprintk("TEMPMONIDET1	  = 0x%8x\n", DRV_Reg32(TEMPMONIDET1));
	tscpu_dprintk("TEMPMONIDET2	  = 0x%8x\n", DRV_Reg32(TEMPMONIDET2));
	tscpu_dprintk("TEMPH2NTHRE	  = 0x%8x\n", DRV_Reg32(TEMPH2NTHRE));
	tscpu_dprintk("TEMPHTHRE	  = 0x%8x\n", DRV_Reg32(TEMPHTHRE));
	tscpu_dprintk("TEMPCTHRE	  = 0x%8x\n", DRV_Reg32(TEMPCTHRE));
	tscpu_dprintk("TEMPOFFSETH	  = 0x%8x\n", DRV_Reg32(TEMPOFFSETH));

	tscpu_dprintk("TEMPOFFSETL	  = 0x%8x\n", DRV_Reg32(TEMPOFFSETL));
	tscpu_dprintk("TEMPMSRCTL0	  = 0x%8x\n", DRV_Reg32(TEMPMSRCTL0));
	tscpu_dprintk("TEMPMSRCTL1	  = 0x%8x\n", DRV_Reg32(TEMPMSRCTL1));
	tscpu_dprintk("TEMPAHBPOLL	  = 0x%8x\n", DRV_Reg32(TEMPAHBPOLL));
	tscpu_dprintk("TEMPAHBTO	  = 0x%8x\n", DRV_Reg32(TEMPAHBTO));
	tscpu_dprintk("TEMPADCPNP0	  = 0x%8x\n", DRV_Reg32(TEMPADCPNP0));

	tscpu_dprintk("TEMPADCPNP1	  = 0x%8x\n", DRV_Reg32(TEMPADCPNP1));
	tscpu_dprintk("TEMPADCPNP2	  = 0x%8x\n", DRV_Reg32(TEMPADCPNP2));
	tscpu_dprintk("TEMPADCMUX	  = 0x%8x\n", DRV_Reg32(TEMPADCMUX));
	tscpu_dprintk("TEMPADCEXT	  = 0x%8x\n", DRV_Reg32(TEMPADCEXT));
	tscpu_dprintk("TEMPADCEXT1	  = 0x%8x\n", DRV_Reg32(TEMPADCEXT1));
	tscpu_dprintk("TEMPADCEN	  = 0x%8x\n", DRV_Reg32(TEMPADCEN));


	tscpu_dprintk("TEMPPNPMUXADDR      = 0x%8x\n", DRV_Reg32(TEMPPNPMUXADDR));
	tscpu_dprintk("TEMPADCMUXADDR      = 0x%8x\n", DRV_Reg32(TEMPADCMUXADDR));
	tscpu_dprintk("TEMPADCEXTADDR      = 0x%8x\n", DRV_Reg32(TEMPADCEXTADDR));
	tscpu_dprintk("TEMPADCEXT1ADDR     = 0x%8x\n", DRV_Reg32(TEMPADCEXT1ADDR));
	tscpu_dprintk("TEMPADCENADDR       = 0x%8x\n", DRV_Reg32(TEMPADCENADDR));
	tscpu_dprintk("TEMPADCVALIDADDR    = 0x%8x\n", DRV_Reg32(TEMPADCVALIDADDR));

	tscpu_dprintk("TEMPADCVOLTADDR     = 0x%8x\n", DRV_Reg32(TEMPADCVOLTADDR));
	tscpu_dprintk("TEMPRDCTRL          = 0x%8x\n", DRV_Reg32(TEMPRDCTRL));
	tscpu_dprintk("TEMPADCVALIDMASK    = 0x%8x\n", DRV_Reg32(TEMPADCVALIDMASK));
	tscpu_dprintk("TEMPADCVOLTAGESHIFT = 0x%8x\n", DRV_Reg32(TEMPADCVOLTAGESHIFT));
	tscpu_dprintk("TEMPADCWRITECTRL    = 0x%8x\n", DRV_Reg32(TEMPADCWRITECTRL));
	tscpu_dprintk("TEMPMSR0            = 0x%8x\n", DRV_Reg32(TEMPMSR0));


	tscpu_dprintk("TEMPIMMD0           = 0x%8x\n", DRV_Reg32(TEMPIMMD0));
	tscpu_dprintk("TEMPIMMD1           = 0x%8x\n", DRV_Reg32(TEMPIMMD1));
	tscpu_dprintk("TEMPIMMD2           = 0x%8x\n", DRV_Reg32(TEMPIMMD2));
	tscpu_dprintk("TEMPPROTCTL         = 0x%8x\n", DRV_Reg32(TEMPPROTCTL));

	tscpu_dprintk("TEMPPROTTA          = 0x%8x\n", DRV_Reg32(TEMPPROTTA));
	tscpu_dprintk("TEMPPROTTB		  = 0x%8x\n", DRV_Reg32(TEMPPROTTB));
	tscpu_dprintk("TEMPPROTTC		  = 0x%8x\n", DRV_Reg32(TEMPPROTTC));
	tscpu_dprintk("TEMPSPARE0		  = 0x%8x\n", DRV_Reg32(TEMPSPARE0));
	tscpu_dprintk("TEMPSPARE1		  = 0x%8x\n", DRV_Reg32(TEMPSPARE1));
	tscpu_dprintk("TEMPSPARE2		  = 0x%8x\n", DRV_Reg32(TEMPSPARE2));
	tscpu_dprintk("TEMPSPARE3		  = 0x%8x\n", DRV_Reg32(TEMPSPARE3));

}

/* TODO: FIXME */
void get_thermal_slope_intercept(struct TS_PTPOD *ts_info, enum thermal_bank_name ts_bank)
{
	unsigned int temp0, temp1, temp2;
	struct TS_PTPOD ts_ptpod;
	S32 x_roomt;

	tscpu_dprintk("get_thermal_slope_intercept\n");

	switch (ts_bank) {
	case THERMAL_BANK0:	/* CPU (TS_MCU1,TS_MCU2)            (TS1,2) */
			if (CPU_TS_MCU1_T > CPU_TS_MCU2_T)
				x_roomt = g_x_roomt[0];	/* TS_MCU1 */
			else
				x_roomt = g_x_roomt[1];	/* TS_MCU2 */
			break;
	case THERMAL_BANK1:	/* GPU (TS_MCU2)                                        (TS2) */
			x_roomt = g_x_roomt[1];	/* TS_MCU2 */
			break;
	case THERMAL_BANK2:	/* SOC (TS_MCU3)    (TS3) */
			x_roomt = g_x_roomt[2];	/* TS_MCU2 */
			break;
	default:		/* choose high temp */
			x_roomt = g_x_roomt[1];
			break;
	}

	temp0 = (10000 * 100000 / g_gain) * 15 / 18;

	if (g_o_slope_sign == 0)
		temp1 = temp0 / (165 + g_o_slope);
	else
		temp1 = temp0 / (165 - g_o_slope);
	ts_ptpod.ts_MTS = temp1;

	temp0 = (g_degc_cali * 10 / 2);
	temp1 = ((10000 * 100000 / 4096 / g_gain) * g_oe + x_roomt * 10) * 15 / 18;

	if (g_o_slope_sign == 0)
		temp2 = temp1 * 10 / (165 + g_o_slope);
	else
		temp2 = temp1 * 10 / (165 - g_o_slope);

	ts_ptpod.ts_BTS = (temp0 + temp2 - 250) * 4 / 10;


	ts_info->ts_MTS = ts_ptpod.ts_MTS;
	ts_info->ts_BTS = ts_ptpod.ts_BTS;
	tscpu_printk("ts_MTS=%d, ts_BTS=%d\n", ts_ptpod.ts_MTS, ts_ptpod.ts_BTS);
}
EXPORT_SYMBOL(get_thermal_slope_intercept);

#if 0
static void dump_spm_reg(void)
{
	/* for SPM reset debug */

	tscpu_printk("SPM_SLEEP_ISR_RAW_STA       =0x%08x\n", spm_read(SPM_SLEEP_ISR_RAW_STA));
	tscpu_printk("SPM_PCM_REG13_DATA          =0x%08x\n", spm_read(SPM_PCM_REG13_DATA));
	tscpu_printk("SPM_SLEEP_WAKEUP_EVENT_MASK =0x%08x\n",
			spm_read(SPM_SLEEP_WAKEUP_EVENT_MASK));
	tscpu_printk("SPM_POWERON_CONFIG_SET      =0x%08x\n", spm_read(SPM_POWERON_CONFIG_SET));
	tscpu_printk("SPM_POWER_ON_VAL1		     =0x%08x\n", spm_read(SPM_POWER_ON_VAL1));
	tscpu_printk("SPM_PCM_IM_LEN			 =0x%08x\n", spm_read(SPM_PCM_IM_LEN));
	tscpu_printk("SPM_PCM_PWR_IO_EN		     =0x%08x\n", spm_read(SPM_PCM_PWR_IO_EN));

	tscpu_printk("SPM_PCM_CON0			     =0x%08x\n", spm_read(SPM_PCM_CON0));
	tscpu_printk("SPM_PCM_CON1			     =0x%08x\n", spm_read(SPM_PCM_CON1));
	tscpu_printk("SPM_PCM_IM_PTR			 =0x%08x\n", spm_read(SPM_PCM_IM_PTR));
	tscpu_printk("SPM_PCM_REG1_DATA			 =0x%08x\n", spm_read(SPM_PCM_REG1_DATA));
	tscpu_printk("SPM_PCM_REG2_DATA			 =0x%08x\n", spm_read(SPM_PCM_REG2_DATA));
	tscpu_printk("SPM_PCM_REG3_DATA			 =0x%08x\n", spm_read(SPM_PCM_REG3_DATA));
	tscpu_printk("SPM_PCM_REG7_DATA			 =0x%08x\n", spm_read(SPM_PCM_REG7_DATA));
	tscpu_printk("SPM_PCM_REG9_DATA			 =0x%08x\n", spm_read(SPM_PCM_REG9_DATA));
	tscpu_printk("SPM_PCM_REG12_DATA			 =0x%08x\n",
			spm_read(SPM_PCM_REG12_DATA));
	tscpu_printk("SPM_PCM_REG14_DATA			 =0x%08x\n",
			spm_read(SPM_PCM_REG14_DATA));
	tscpu_printk("SPM_PCM_REG15_DATA			 =0x%08x\n",
			spm_read(SPM_PCM_REG15_DATA));
	tscpu_printk("SPM_PCM_FSM_STA			 =0x%08x\n", spm_read(SPM_PCM_FSM_STA));


}
#endif


static void thermal_interrupt_handler(int bank)
{
	U32 ret = 0;
	unsigned long flags;

	mt_ptp_lock(&flags);

	mtktscpu_switch_bank(bank);

	ret = DRV_Reg32(TEMPMONINTSTS);
	tscpu_printk("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n");
	tscpu_printk("thermal_interrupt_handler,bank=0x%08x,ret=0x%08x\n", bank, ret);
	tscpu_printk("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n");

	/* ret2 = DRV_Reg32(THERMINTST); */
	/* tscpu_printk("thermal_interrupt_handler : THERMINTST = 0x%x\n", ret2); */

	/* for SPM reset debug */
	/* dump_spm_reg(); */

	/* tscpu_printk("thermal_isr: [Interrupt trigger]: status = 0x%x\n", ret); */
	if (ret & THERMAL_MON_CINTSTS0)
		tscpu_dprintk("thermal_isr: thermal sensor point 0 - cold interrupt trigger\n");

	if (ret & THERMAL_MON_HINTSTS0)
		tscpu_dprintk("<<<thermal_isr>>>: thermal sensor point 0 - hot interrupt trigger\n");

	if (ret & THERMAL_MON_HINTSTS1)
		tscpu_dprintk("<<<thermal_isr>>>: thermal sensor point 1 - hot interrupt trigger\n");

	if (ret & THERMAL_MON_HINTSTS2)
		tscpu_dprintk("<<<thermal_isr>>>: thermal sensor point 2 - hot interrupt trigger\n");


	if (ret & THERMAL_tri_SPM_State0)
		tscpu_dprintk("thermal_isr: Thermal state0 to trigger SPM state0\n");
	if (ret & THERMAL_tri_SPM_State1) {
		/* tscpu_dprintk("thermal_isr: Thermal state1 to trigger SPM state1\n"); */
#if MTK_TS_CPU_RT


		tscpu_dprintk("THERMAL_tri_SPM_State1, T=%d,%d,%d,%d,%d,%d\n", CPU_TS_MCU1_T,
				CPU_TS_MCU2_T, GPU_TS_MCU2_T, SOC_TS_MCU1_T, SOC_TS_MCU2_T,
				SOC_TS_MCU3_T);



		wake_up_process(ktp_thread_handle);
#endif
	}
	if (ret & THERMAL_tri_SPM_State2)
		tscpu_dprintk("thermal_isr: Thermal state2 to trigger SPM state2\n");

	mt_ptp_unlock(&flags);

}

static irqreturn_t thermal_all_bank_interrupt_handler(int irq, void *dev_id)
{
	U32 ret = 0;

	/* tscpu_dprintk("thermal_all_bank_interrupt_handler\n"); */


	ret = DRV_Reg32(THERMINTST);
	ret = ret & 0xF;
	tscpu_printk("thermal_all_bank_interrupt_handler : THERMINTST = 0x%x\n", ret);

	if ((ret & 0x1) == 0)	/* check bit0 */
		thermal_interrupt_handler(THERMAL_BANK0);

	if ((ret & 0x2) == 0)	/* check bit1 */
		thermal_interrupt_handler(THERMAL_BANK1);

	if ((ret & 0x4) == 0)	/* check bit2 */
		thermal_interrupt_handler(THERMAL_BANK2);


	return IRQ_HANDLED;
}

static void thermal_reset_and_initial(void)
{

	/* tscpu_printk( "thermal_reset_and_initial\n"); */

	/* mtktscpu_thermal_clock_on(); */


	/* Calculating period unit in Module clock x 256, and the Module clock */
	/* will be changed to 26M when Infrasys enters Sleep mode. */
	/* THERMAL_WRAP_WR32(0x000003FF, TEMPMONCTL1);     counting unit is 1023 * 15.15ns ~ 15.5us */

	/* bus clock 66M counting unit is 4*15.15ns* 256 = 15513.6 ms=15.5us */
	/* THERMAL_WRAP_WR32(0x00000004, TEMPMONCTL1);*/
	THERMAL_WRAP_WR32(0x0000000C, TEMPMONCTL1);	/* bus clock 66M counting unit is 12*15.15ns* 256 = 46.540us */
	/* bus clock 66M counting unit is 4*15.15ns* 256 = 15513.6 ms=15.5us */
	/* THERMAL_WRAP_WR32(0x000001FF, TEMPMONCTL1);*/

#if THERMAL_CONTROLLER_HW_FILTER == 2
	THERMAL_WRAP_WR32(0x07E007E0, TEMPMONCTL2);	/* both filt and sen interval is 2016*15.5us = 31.25ms */
	THERMAL_WRAP_WR32(0x001F7972, TEMPAHBPOLL);	/* poll is set to 31.25ms */
	THERMAL_WRAP_WR32(0x00000049, TEMPMSRCTL0);	/* temperature sampling control, 2 out of 4 samples */
#elif THERMAL_CONTROLLER_HW_FILTER == 4
	THERMAL_WRAP_WR32(0x050A050A, TEMPMONCTL2);	/* both filt and sen interval is 20ms */
	THERMAL_WRAP_WR32(0x001424C4, TEMPAHBPOLL);	/* poll is set to 20ms */
	THERMAL_WRAP_WR32(0x000000DB, TEMPMSRCTL0);	/* temperature sampling control, 4 out of 6 samples */
#elif THERMAL_CONTROLLER_HW_FILTER == 8
	THERMAL_WRAP_WR32(0x03390339, TEMPMONCTL2);	/* both filt and sen interval is 12.5ms */
	THERMAL_WRAP_WR32(0x000C96FA, TEMPAHBPOLL);	/* poll is set to 12.5ms */
	THERMAL_WRAP_WR32(0x00000124, TEMPMSRCTL0);	/* temperature sampling control, 8 out of 10 samples */
#elif THERMAL_CONTROLLER_HW_FILTER == 16
	THERMAL_WRAP_WR32(0x01C001C0, TEMPMONCTL2);	/* both filt and sen interval is 6.94ms */
	THERMAL_WRAP_WR32(0x0006FE8B, TEMPAHBPOLL);	/* poll is set to 458379*15.15= 6.94ms */
	THERMAL_WRAP_WR32(0x0000016D, TEMPMSRCTL0);	/* temperature sampling control, 16 out of 18 samples */
#else				/* default 1 */
	/* filt interval is 1 * 46.540us = 46.54us, sen interval is 429 * 46.540us = 19.96ms */
	THERMAL_WRAP_WR32(0x000101AD, TEMPMONCTL2);
	/* filt interval is 1 * 46.540us = 46.54us, sen interval is 858 * 46.540us = 39.93ms */
	/* THERMAL_WRAP_WR32(0x0001035A, TEMPMONCTL2); */
	/* filt interval is 1 * 46.540us = 46.54us, sen interval is 1287 * 46.540us = 59.89 ms */
	/* THERMAL_WRAP_WR32(0x00010507, TEMPMONCTL2); */
	/*  poll is set to 1 * 46.540us = 46.540us */
	/* THERMAL_WRAP_WR32(0x00000001, TEMPAHBPOLL); */
	THERMAL_WRAP_WR32(0x00000300, TEMPAHBPOLL);	/* poll is set to 10u */
	THERMAL_WRAP_WR32(0x00000000, TEMPMSRCTL0);	/* temperature sampling control, 1 sample */
#endif

	THERMAL_WRAP_WR32(0xFFFFFFFF, TEMPAHBTO);	/* exceed this polling time, IRQ would be inserted */

	THERMAL_WRAP_WR32(0x00000000, TEMPMONIDET0);	/* times for interrupt occurrance */
	THERMAL_WRAP_WR32(0x00000000, TEMPMONIDET1);	/* times for interrupt occurrance */


	THERMAL_WRAP_WR32(0x800, TEMPADCMUX);
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */
	THERMAL_WRAP_WR32((UINT32) AUXADC_CON1_CLR_P, TEMPADCMUXADDR);
	/* AHB address for auxadc mux selection */
	/* THERMAL_WRAP_WR32(0x1100100C, TEMPADCMUXADDR); AHB address for auxadc mux selection */

	THERMAL_WRAP_WR32(0x800, TEMPADCEN);	/* AHB value for auxadc enable */
	THERMAL_WRAP_WR32((UINT32) AUXADC_CON1_SET_P, TEMPADCENADDR);
	/* AHB address for auxadc enable (channel 0 immediate mode selected) */
	/* THERMAL_WRAP_WR32(0x11001008, TEMPADCENADDR);  AHB address for auxadc enable
	(channel 0 immediate mode selected) this value will be stored to TEMPADCENADDR automatically by hw */


	THERMAL_WRAP_WR32((UINT32) AUXADC_DAT11_P, TEMPADCVALIDADDR);	/* AHB address for auxadc valid bit */
	THERMAL_WRAP_WR32((UINT32) AUXADC_DAT11_P, TEMPADCVOLTADDR);	/* AHB address for auxadc voltage output */
	/* THERMAL_WRAP_WR32(0x11001040, TEMPADCVALIDADDR);  AHB address for auxadc valid bit */
	/* THERMAL_WRAP_WR32(0x11001040, TEMPADCVOLTADDR);   AHB address for auxadc voltage output */


	THERMAL_WRAP_WR32(0x0, TEMPRDCTRL);	/* read valid & voltage are at the same register */
	/* indicate where the valid bit is (the 12th bit is valid bit and 1 is valid) */
	THERMAL_WRAP_WR32(0x0000002C, TEMPADCVALIDMASK);
	THERMAL_WRAP_WR32(0x0, TEMPADCVOLTAGESHIFT);	/* do not need to shift */
	/* THERMAL_WRAP_WR32(0x2, TEMPADCWRITECTRL);*/
	/* enable auxadc mux write transaction */

}

/* Bank0 : CPU (TS_MCU1,TS_MCU2) (TS3, TS4) */
static void thermal_enable_all_periodoc_sensing_point_Bank0(void)
{
	THERMAL_WRAP_WR32(0x00000003, TEMPMONCTL0);	/* enable periodoc temperature sensing point 0, point 1 */
}

/* Bank1 : GPU (TS_MCU3) (TS5), ABB (TS_ABB) */
static void thermal_enable_all_periodoc_sensing_point_Bank1(void)
{
	THERMAL_WRAP_WR32(0x00000003, TEMPMONCTL0);	/* enable periodoc temperature sensing point 0, point 1 */
}

/* Bank2 : SoC (TS_MCU4,TS_MCU2,TS_MCU3) (TS1, TS4, TS5) */
static void thermal_enable_all_periodoc_sensing_point_Bank2(void)
{
	/* enable periodoc temperature sensing point 0, point 1, point 2 */
	THERMAL_WRAP_WR32(0x0000000F, TEMPMONCTL0);
}


/*
Bank0 : CPU (TS_MCU1,TS_MCU2)        (TS3, TS4)
Bank1 : GPU (TS_MCU3)                (TS5)
Bank2 : SOC (TS_MCU4,TS_MCU2,TS_MCU3)(TS1, TS4, TS5)
 */

static void thermal_config_Bank0_TS(void)
{
	/* tscpu_dprintk( "thermal_config_Bank0_TS:\n"); */


	/* Bank0:CPU(TS_MCU1 and TS_MCU2) */
	/* TSCON1[5:4]=2'b00 */
	/* TSCON1[2:0]=3'b000 */
	THERMAL_WRAP_WR32(0x0, TEMPADCPNP0);
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */

	/* TSCON1[5:4]=2'b00 */
	/* TSCON1[2:0]=3'b001 */
	THERMAL_WRAP_WR32(0x1, TEMPADCPNP1);
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */

	THERMAL_WRAP_WR32(TS_CON1_P, TEMPPNPMUXADDR);	/* AHB address for pnp sensor mux selection */

	THERMAL_WRAP_WR32(0x3, TEMPADCWRITECTRL);
	/*      enable periodoc temperature sensing point 0, point 1 */
	/* THERMAL_WRAP_WR32(0x00000003, TEMPMONCTL0);*/
}

static void thermal_config_Bank1_TS(void)
{

	/* tscpu_dprintk( "thermal_config_Bank1_TS\n"); */

	/* Bank1:GPU(TS_MCU3) */
	/* TSCON1[5:4]=2'b00 */
	/* TSCON1[2:0]=3'b010 */
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */
	THERMAL_WRAP_WR32(0x2, TEMPADCPNP0);

	/* Bank1:ABB(TS_ABB) */
	/* TSCON1[5:4]=2'b01 */
	/* TSCON1[2:0]=3'b000 */
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */
	THERMAL_WRAP_WR32(0x10, TEMPADCPNP1);

	THERMAL_WRAP_WR32(TS_CON1_P, TEMPPNPMUXADDR);	/* AHB address for pnp sensor mux selection */

	THERMAL_WRAP_WR32(0x3, TEMPADCWRITECTRL);
	/* enable periodoc temperature sensing point 0, point 1 */
	/* THERMAL_WRAP_WR32(0x00000003, TEMPMONCTL0);*/
}

static void thermal_config_Bank2_TS(void)
{

	/* tscpu_dprintk( "thermal_config_Bank2_TS\n"); */

	/* Bank1:SOC(TS_MCU4,TS_MCU2,TS_MCU3) */

	/* TSCON1[5:4]=2'b00 */
	/* TSCON1[2:0]=3'b011 */
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */
	THERMAL_WRAP_WR32(0x3, TEMPADCPNP0);

	/* TSCON1[5:4]=2'b00 */
	/* TSCON1[2:0]=3'b001 */
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */
	THERMAL_WRAP_WR32(0x1, TEMPADCPNP1);

	/* TSCON1[5:4]=2'b00 */
	/* TSCON1[2:0]=3'b010 */
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */
	THERMAL_WRAP_WR32(0x2, TEMPADCPNP2);

	/* Bank1:ABB(TS_ABB) */
	/* TSCON1[5:4]=2'b01 */
	/* TSCON1[2:0]=3'b000 */
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */
	THERMAL_WRAP_WR32(0x10, TEMPADCPNP3);

	THERMAL_WRAP_WR32(TS_CON1_P, TEMPPNPMUXADDR);	/* AHB address for pnp sensor mux selection */

	THERMAL_WRAP_WR32(0x3, TEMPADCWRITECTRL);
	/* enable periodoc temperature sensing point 0, point 1, point 2 */
	/* THERMAL_WRAP_WR32(0x00000003, TEMPMONCTL0);*/

}



static void thermal_config_TS_in_banks(enum thermal_bank_name bank_num)
{
	/* tscpu_dprintk( "thermal_config_TS_in_banks bank_num=%d\n",bank_num); */

	switch (bank_num) {
	case THERMAL_BANK0:	/* CPU(TS_MCU1 and TS_MCU2) */
			thermal_config_Bank0_TS();
			break;
	case THERMAL_BANK1:	/* GPU (TS_MCU3), ABB(TS_ABB) */
			thermal_config_Bank1_TS();
			break;
	case THERMAL_BANK2:	/* SOC(TS_MCU4,TS_MCU2,TS_MCU3) */
			thermal_config_Bank2_TS();
			break;
	default:
			thermal_config_Bank0_TS();	/* CPU(TS_MCU1 and TS_MCU2) */
			break;
	}
}



static void thermal_enable_all_periodoc_sensing_point(enum thermal_bank_name bank_num)
{
	/* tscpu_dprintk( "thermal_config_TS_in_banks bank_num=%d\n",bank_num); */

	switch (bank_num) {
	case THERMAL_BANK0:	/* CPU(TS_MCU1 and TS_MCU2) */
			thermal_enable_all_periodoc_sensing_point_Bank0();
			break;
	case THERMAL_BANK1:	/* GPU (TS_MCU3), ABB(TS_ABB) */
			thermal_enable_all_periodoc_sensing_point_Bank1();
			break;
	case THERMAL_BANK2:	/* SOC(TS_MCU4,TS_MCU2,TS_MCU3) */
			thermal_enable_all_periodoc_sensing_point_Bank2();
			break;
	default:
			thermal_enable_all_periodoc_sensing_point_Bank0();
			break;
	}
}



#if 0
static void set_tc_trigger_hw_protect_test(int temp_hot, int temp_normal, int temp_low, int loffset)
{
	int temp = 0;
	int offset = 0;
	int raw_high, raw_middle, raw_low;
	int raw_high_offset, raw_hot_normal, raw_low_offset;

	tscpu_dprintk
		("set_tc_trigger_hw_protect_test temp_hot=%d, temp_normal=%d, temp_low=%d, loffset=%d\n",
		 temp_hot, temp_normal, temp_low, loffset);

	offset = temperature_to_raw_room(loffset);
	raw_high = temperature_to_raw_room(temp_hot);
	raw_hot_normal = temperature_to_raw_room(temp_normal);
	raw_low = temperature_to_raw_room(temp_low);

	raw_high_offset = raw_high + offset;
	raw_hot_normal = raw_high_offset + offset;
	raw_low_offset = raw_hot_normal + offset;

	temp = DRV_Reg32(TEMPMONINT);
	tscpu_dprintk("set_tc_trigger_hw_protect_test 1 TEMPMONINT:temp=0x%x\n", temp);


	THERMAL_WRAP_WR32(raw_high, TEMPHTHRE);	/* set hot threshold */
	/* THERMAL_WRAP_WR32(raw_high_offset, TEMPOFFSETH);     set high offset threshold */
	THERMAL_WRAP_WR32(raw_hot_normal, TEMPH2NTHRE);	/* set hot to normal threshold */
	/* THERMAL_WRAP_WR32(raw_low_offset,  TEMPOFFSETL);     set low offset threshold */
	THERMAL_WRAP_WR32(raw_low, TEMPCTHRE);	/* set cold threshold */

	THERMAL_WRAP_WR32(temp | 0x00004E73, TEMPMONINT);	/* enable trigger middle & Hot SPM interrupt */
	tscpu_dprintk("set_tc_trigger_hw_protect_test 2 TEMPMONINT:temp=0x%x\n", temp);

}
#endif
#if 0				/* test int */
static void set_thermal_ctrl_trigger_SPM(int temperature, int temperature2)
{
#if THERMAL_CONTROLLER_HW_TP
	int temp = 0;
	int raw_high, raw_middle, raw_low;

	tscpu_dprintk("set_thermal_ctrl_trigger_SPM t11=%d t22=%d\n", temperature, temperature2);

	raw_high = temperature_to_raw_room(80000);
	raw_middle = temperature_to_raw_room(60000);
	raw_low = temperature_to_raw_room(45000);

	temp = DRV_Reg32(TEMPMONINT);
	tscpu_dprintk("TEMPMONINT:1 temp=0x%x\n", temp);
	/* THERMAL_WRAP_WR32(temp & 0x1FFFFFFF, TEMPMONINT);      disable trigger SPM interrupt */
	/* THERMAL_WRAP_WR32(temp & 0x00000000, TEMPMONINT);      disable trigger SPM interrupt */
	tscpu_dprintk("TEMPMONINT:2 temp=0x%x\n", temp);

	tscpu_dprintk("raw_high=%d,raw_middle=%d,raw_low=%d\n", raw_high, raw_middle, raw_low);

	tscpu_dprintk("high=%d,middle=%d,low=%d\n", 80000, 60000, 45000);


	/* THERMAL_WRAP_WR32(0x20000, TEMPPROTCTL); set hot to wakeup event control */
	/* THERMAL_WRAP_WR32(raw_low, TEMPPROTTA); */
	/* if (temperature2 > -275000) */
	/* THERMAL_WRAP_WR32(raw_middle, TEMPPROTTB);  register will remain unchanged if -275000... */
	/* THERMAL_WRAP_WR32(raw_high, TEMPPROTTC); set hot to HOT wakeup event */



	THERMAL_WRAP_WR32(raw_high, TEMPHTHRE);	/* set hot threshold */
	THERMAL_WRAP_WR32(raw_middle, TEMPH2NTHRE);	/* set hot to normal */
	THERMAL_WRAP_WR32(raw_low, TEMPCTHRE);	/* set cold threshold */


	/*trigger cold ,normal and hot interrupt */
	/* remove for temp  THERMAL_WRAP_WR32(temp | 0xE0000000, TEMPMONINT); enable trigger SPM interrupt */
	/*Only trigger hot interrupt */
	/* if (temperature2 > -275000) */
	/* THERMAL_WRAP_WR32(temp | 0x00000842, TEMPMONINT);  enable trigger Hot SPM interrupt */
	/* else */
	THERMAL_WRAP_WR32(temp | 0x00000842, TEMPMONINT);	/* enable trigger Hot SPM interrupt */

	tscpu_dprintk("TEMPMONINT:3 temp=0x%x\n", DRV_Reg32(TEMPMONINT));
#endif
}

#endif

/**
 *  temperature2 to set the middle threshold for interrupting CPU. -275000 to disable it.
 */


static void set_tc_trigger_hw_protect(int temperature, int temperature2)
{
	int temp = 0;
	int raw_high, raw_middle, raw_low;


	/* temperature2=80000;  test only */
	tscpu_dprintk("set_tc_trigger_hw_protect t1=%d t2=%d\n", temperature, temperature2);


	/* temperature to trigger SPM state2 */
	raw_high = temperature_to_raw_room(temperature);
	if (temperature2 > -275000)
		raw_middle = temperature_to_raw_room(temperature2);
	raw_low = temperature_to_raw_room(5000);


	temp = DRV_Reg32(TEMPMONINT);
	/* tscpu_dprintk("set_tc_trigger_hw_protect 1 TEMPMONINT:temp=0x%x\n",temp); */
	/* THERMAL_WRAP_WR32(temp & 0x1FFFFFFF, TEMPMONINT);      disable trigger SPM interrupt */
	THERMAL_WRAP_WR32(temp & 0x00000000, TEMPMONINT);	/* disable trigger SPM interrupt */


	/* THERMAL_WRAP_WR32(0x60000, TEMPPROTCTL); set hot to wakeup event control */
	/* THERMAL_WRAP_WR32(0x20000, TEMPPROTCTL); set hot to wakeup event control */
	THERMAL_WRAP_WR32(0x20000, TEMPPROTCTL);	/* set hot to wakeup event control */

	THERMAL_WRAP_WR32(raw_low, TEMPPROTTA);
	if (temperature2 > -275000)
		THERMAL_WRAP_WR32(raw_middle, TEMPPROTTB);	/* register will remain unchanged if -275000... */


	THERMAL_WRAP_WR32(raw_high, TEMPPROTTC);	/* set hot to HOT wakeup event */


	/*trigger cold ,normal and hot interrupt */
	/* remove for temp       THERMAL_WRAP_WR32(temp | 0xE0000000, TEMPMONINT);
	 enable trigger SPM interrupt */
	/*Only trigger hot interrupt */
	if (temperature2 > -275000)
		THERMAL_WRAP_WR32(temp | 0xC0000000, TEMPMONINT);	/* enable trigger middle & Hot SPM interrupt */
	else
		THERMAL_WRAP_WR32(temp | 0x80000000, TEMPMONINT);	/* enable trigger Hot SPM interrupt */

#if 0				/* debug */
	{
		int cunt = 0, temp1 = 0;

		cunt = 0;
		temp1 = DRV_Reg32(TEMPMSR0) & 0x0fff;
		/* if(cunt==0)  tscpu_dprintk("[Power/CPU_Thermal]0 hw_protect temp=%d\n",temp1); */
		while (temp1 <= 3000 && cunt < 20) {
			cunt++;
			tscpu_dprintk("[Power/CPU_Thermal]0 hw_protect temp=%d%d\n", temp1, __LINE__);
			temp1 = DRV_Reg32(TEMPMSR0) & 0x0fff;
			tscpu_dprintk("TS_CON1=0x%x\n", DRV_Reg32(TS_CON1));
			udelay(2);
		}

		cunt = 0;
		temp1 = DRV_Reg32(TEMPMSR1) & 0x0fff;
		/* if(cunt==0)  tscpu_dprintk("[Power/CPU_Thermal]1 hw_protect temp=%d\n",temp1); */
		while (temp1 <= 3000 && cunt < 20) {
			cunt++;
			tscpu_dprintk("[Power/CPU_Thermal]1 hw_protect temp=%d%d\n", temp1, __LINE__);
			temp1 = DRV_Reg32(TEMPMSR1) & 0x0fff;
			tscpu_dprintk("TS_CON1=0x%x\n", DRV_Reg32(TS_CON1));
			udelay(2);
		}

		cunt = 0;
		temp1 = DRV_Reg32(TEMPMSR2) & 0x0fff;
		/* if(cunt==0) tscpu_dprintk("[Power/CPU_Thermal]2 hw_protect temp=%d\n",temp1); */
		while (temp1 <= 3000 && cunt < 20) {
			cunt++;
			tscpu_dprintk("[Power/CPU_Thermal]2 hw_protect temp=%d%d\n", temp1, __LINE__);
			temp1 = DRV_Reg32(TEMPMSR2) & 0x0fff;
			tscpu_dprintk("TS_CON1=0x%x\n", DRV_Reg32(TS_CON1));
			udelay(2);
		}

		cunt = 0;
		temp1 = DRV_Reg32(TEMPMSR3) & 0x0fff;
		/* if(cunt==0)    tscpu_dprintk("[Power/CPU_Thermal]3 hw_protect temp=%d\n",temp1); */
		while (temp1 <= 3000 && cunt < 20) {
			cunt++;
			tscpu_dprintk("[Power/CPU_Thermal]3 hw_protect temp=%d,%d\n", temp1, __LINE__);
			temp1 = DRV_Reg32(TEMPMSR3) & 0x0fff;
			tscpu_dprintk("TS_CON1=0x%x\n", DRV_Reg32(TS_CON1));
			udelay(2);
		}

	}
#endif

	/* tscpu_dprintk("set_tc_trigger_hw_protect 2 TEMPMONINT:temp=0x%x\n",temp); */

}



int mtk_cpufreq_register(struct mtk_cpu_power_info *freqs, int num)
{
	int i = 0;
	int gpu_power = 0;

	tscpu_dprintk("mtk_cpufreq_register\n");

	tscpu_num_opp = num;

	mtk_cpu_power = kzalloc((num) * sizeof(struct mtk_cpu_power_info), GFP_KERNEL);
	if (mtk_cpu_power == NULL)
		return -ENOMEM;

	/* WARNING: here assumes mtk_gpufreq_register is always called first.
	If not, here the tscpu_cpu_dmips is wrong... */
	if (0 != Num_of_GPU_OPP && NULL != mtk_gpu_power)
		gpu_power = mtk_gpu_power[Num_of_GPU_OPP - 1].gpufreq_power;
	else
		tscpu_printk("Num_of_GPU_OPP is 0!\n");

#if 0
	if (Num_of_GPU_OPP == 3)
		gpu_power = mtk_gpu_power[2].gpufreq_power;
	else if (Num_of_GPU_OPP == 2)
		gpu_power = mtk_gpu_power[1].gpufreq_power;
	else			/* Num_of_GPU_OPP=1 */
		gpu_power = mtk_gpu_power[0].gpufreq_power;
#endif

	for (i = 0; i < num; i++) {
		int dmips = freqs[i].cpufreq_khz * freqs[i].cpufreq_ncpu / 1000;
		/* TODO: this line must be modified every time cooler mapping table changes */
		int cl_id = (((freqs[i].cpufreq_power + gpu_power) + 99) / 100) - 7;

		mtk_cpu_power[i].cpufreq_khz = freqs[i].cpufreq_khz;
		mtk_cpu_power[i].cpufreq_ncpu = freqs[i].cpufreq_ncpu;
		mtk_cpu_power[i].cpufreq_power = freqs[i].cpufreq_power;

		if (cl_id < CPU_COOLER_NUM) {
			if (tscpu_cpu_dmips[cl_id] < dmips)
				tscpu_cpu_dmips[cl_id] = dmips;
		}

	}

	{
		/* TODO: this assumes the last OPP consumes least power...
				need to check this every time OPP table changes */
		int base =
			(mtk_cpu_power[num - 1].cpufreq_khz * mtk_cpu_power[num - 1].cpufreq_ncpu) /
			1000;
		for (i = 0; i < CPU_COOLER_NUM; i++) {
			if (0 == tscpu_cpu_dmips[i] || tscpu_cpu_dmips[i] < base)
				tscpu_cpu_dmips[i] = base;
			else
				base = tscpu_cpu_dmips[i];
		}
		mtktscpu_limited_dmips = base;
	}

	return 0;
}
EXPORT_SYMBOL(mtk_cpufreq_register);

/* Init local structure for AP coolers */
static int init_cooler(void)
{
	int i;
	int num = CPU_COOLER_NUM;	/* 700~4000, 92 */

	cl_dev_state = kzalloc((num) * sizeof(unsigned int), GFP_KERNEL);
	if (cl_dev_state == NULL)
		return -ENOMEM;

	cl_dev = kzalloc((num) * sizeof(struct thermal_cooling_device *),	GFP_KERNEL);
	if (cl_dev == NULL)
		return -ENOMEM;

	cooler_name = kzalloc((num) * sizeof(char) * 20, GFP_KERNEL);
	if (cooler_name == NULL)
		return -ENOMEM;

	for (i = 0; i < num; i++)
		sprintf(cooler_name + (i * 20), "cpu%02d", i);	/* using index=>0=700,1=800 ~ 33=4000 */

	Num_of_OPP = num;	/* CPU COOLER COUNT, not CPU OPP count */
	return 0;
}

int mtk_gpufreq_register(struct mtk_gpu_power_info *freqs, int num)
{
	int i = 0;

	tscpu_dprintk("mtk_gpufreq_register\n");
	mtk_gpu_power = kzalloc((num) * sizeof(struct mtk_gpu_power_info), GFP_KERNEL);
	if (mtk_gpu_power == NULL)
		return -ENOMEM;

	for (i = 0; i < num; i++) {
		mtk_gpu_power[i].gpufreq_khz = freqs[i].gpufreq_khz;
		mtk_gpu_power[i].gpufreq_power = freqs[i].gpufreq_power;

		tscpu_dprintk("[%d].gpufreq_khz=%u, .gpufreq_power=%u\n",
				i, freqs[i].gpufreq_khz, freqs[i].gpufreq_power);
	}

	Num_of_GPU_OPP = num;	/* GPU OPP count */
	return 0;
}
EXPORT_SYMBOL(mtk_gpufreq_register);

void mtkts_dump_cali_info(void)
{
	tscpu_dprintk("[cal] g_adc_ge_t      = 0x%x\n", g_adc_ge_t);
	tscpu_dprintk("[cal] g_adc_oe_t      = 0x%x\n", g_adc_oe_t);
	tscpu_dprintk("[cal] g_degc_cali     = 0x%x\n", g_degc_cali);
	tscpu_dprintk("[cal] g_adc_cali_en_t = 0x%x\n", g_adc_cali_en_t);
	tscpu_dprintk("[cal] g_o_slope       = 0x%x\n", g_o_slope);
	tscpu_dprintk("[cal] g_o_slope_sign  = 0x%x\n", g_o_slope_sign);
	tscpu_dprintk("[cal] g_id            = 0x%x\n", g_id);

	tscpu_dprintk("[cal] g_o_vtsmcu2     = 0x%x\n", g_o_vtsmcu2);
	tscpu_dprintk("[cal] g_o_vtsmcu3     = 0x%x\n", g_o_vtsmcu3);
	tscpu_dprintk("[cal] g_o_vtsmcu4     = 0x%x\n", g_o_vtsmcu4);
}


static void thermal_cal_prepare(void)
{
	U32 temp0 = 0, temp1 = 0, temp2 = 0;


	/* temp0 = DRV_Reg32(0xF0206184); */
	/* temp1 = DRV_Reg32(0xF0206180); */
	/* temp2 = DRV_Reg32(0xF0206188); */

	temp0 = get_devinfo_with_index(8);
	temp1 = get_devinfo_with_index(7);
	temp2 = get_devinfo_with_index(9);


	tscpu_dprintk("[calibration] devinfo idx7=0x%x, idx8=0x%x, idx9=0x%x\n", temp1, temp0,
			temp2);
	/* mtktscpu_dprintk("thermal_cal_prepare\n"); */

	g_adc_ge_t = ((temp0 & 0xFFC00000) >> 22);	/* ADC_GE_T    [9:0] *(0x10206184)[31:22] */
	g_adc_oe_t = ((temp0 & 0x003FF000) >> 12);	/* ADC_OE_T    [9:0] *(0x10206184)[21:12] */

	g_o_vtsmcu1 = (temp1 & 0x03FE0000) >> 17;	/* O_VTSMCU1    (9b) *(0x10206180)[25:17] */
	g_o_vtsmcu2 = (temp1 & 0x0001FF00) >> 8;	/* O_VTSMCU2    (9b) *(0x10206180)[16:8] */
	g_o_vtsmcu3 = (temp0 & 0x000001FF);	/* O_VTSMCU3    (9b) *(0x10206184)[8:0] */
	g_o_vtsmcu4 = (temp2 & 0xFF800000) >> 23;	/* O_VTSMCU4    (9b) *(0x10206188)[31:23] */
	g_o_vtsabb = (temp2 & 0x007FC000) >> 14;	/* O_VTSABB     (9b) *(0x10206188)[22:14] */

	g_degc_cali = (temp1 & 0x0000007E) >> 1;	/* DEGC_cali    (6b) *(0x10206180)[6:1] */
	g_adc_cali_en_t = (temp1 & 0x00000001);	/* ADC_CALI_EN_T(1b) *(0x10206180)[0] */
	g_o_slope_sign = (temp1 & 0x00000080) >> 7;	/* O_SLOPE_SIGN (1b) *(0x10206180)[7] */
	g_o_slope = (temp1 & 0xFC000000) >> 26;	/* O_SLOPE      (6b) *(0x10206180)[31:26] */

	g_id = (temp0 & 0x00000200) >> 9;	/* ID           (1b) *(0x10206184)[9] */

	/*
	   Check ID bit
	   If ID=0 (TSMC sample)    , ignore O_SLOPE EFuse value and set O_SLOPE=0.
	   If ID=1 (non-TSMC sample), read O_SLOPE EFuse value for following calculation.
	 */
	if (g_id == 0)
		g_o_slope = 0;

	/* g_adc_cali_en_t=0;test only */
	if (g_adc_cali_en_t == 1) {
		/* thermal_enable = true; */
	} else {
		tscpu_dprintk("This sample is not Thermal calibrated\n");
		g_adc_ge_t = 512;
		g_adc_oe_t = 512;
		g_degc_cali = 40;
		g_o_slope = 0;
		g_o_slope_sign = 0;
		g_o_vtsmcu1 = 260;
		g_o_vtsmcu2 = 260;
		g_o_vtsmcu3 = 260;
		g_o_vtsmcu4 = 260;
		g_o_vtsabb = 260;

	}

	mtkts_dump_cali_info();
}

static void thermal_cal_prepare_2(U32 ret)
{
	S32 format_1 = 0, format_2 = 0, format_3 = 0, format_4 = 0, format_5 = 0;

	/* tscpu_printk("thermal_cal_prepare_2\n"); */

	g_ge = ((g_adc_ge_t - 512) * 10000) / 4096;	/* ge * 10000 */
	g_oe = (g_adc_oe_t - 512);

	g_gain = (10000 + g_ge);

	format_1 = (g_o_vtsmcu1 + 3350 - g_oe);
	format_2 = (g_o_vtsmcu2 + 3350 - g_oe);
	format_3 = (g_o_vtsmcu3 + 3350 - g_oe);
	format_4 = (g_o_vtsmcu4 + 3350 - g_oe);
	format_5 = (g_o_vtsabb + 3350 - g_oe);

	g_x_roomt[0] = (((format_1 * 10000) / 4096) * 10000) / g_gain;	/* g_x_roomt1 * 10000 */
	g_x_roomt[1] = (((format_2 * 10000) / 4096) * 10000) / g_gain;	/* g_x_roomt2 * 10000 */
	g_x_roomt[2] = (((format_3 * 10000) / 4096) * 10000) / g_gain;	/* g_x_roomt3 * 10000 */

	tscpu_dprintk("[cal] g_ge         = 0x%x\n", g_ge);
	tscpu_dprintk("[cal] g_gain       = 0x%x\n", g_gain);

	tscpu_dprintk("[cal] g_x_roomt1   = 0x%x\n", g_x_roomt[0]);
	tscpu_dprintk("[cal] g_x_roomt2   = 0x%x\n", g_x_roomt[1]);
	tscpu_dprintk("[cal] g_x_roomt3   = 0x%x\n", g_x_roomt[2]);

}

#if THERMAL_CONTROLLER_HW_TP

static S32 temperature_to_raw_room(U32 ret)
{
	/* Ycurr = [(Tcurr - DEGC_cali/2)*(165+O_slope)*(18/15)*(1/10000)+X_roomtabb]*Gain*4096 + OE */

	S32 t_curr = ret;
	S32 format_1 = 0;
	S32 format_2 = 0;
	S32 format_3[THERMAL_SENSOR_NUM] = { 0 };
	S32 format_4[THERMAL_SENSOR_NUM] = { 0 };
	S32 i, index = 0, temp = 0;


	/* tscpu_dprintk("temperature_to_raw_room\n"); */

	if (g_o_slope_sign == 0) {	/* O_SLOPE is Positive. */
		format_1 = t_curr - (g_degc_cali * 1000 / 2);
		format_2 = format_1 * (165 + g_o_slope) * 18 / 15;
		format_2 = format_2 - 2 * format_2;

		for (i = 0; i < THERMAL_SENSOR_NUM; i++) {
			format_3[i] = format_2 / 1000 + g_x_roomt[i] * 10;
			format_4[i] = (format_3[i] * 4096 / 10000 * g_gain) / 100000 + g_oe;
			/* tscpu_dprintk("[Temperature_to_raw_roomt][roomt%d] format_1=%d,
format_2=%d, format_3=%d, format_4=%d\n",
i, format_1, format_2, format_3[i], format_4[i]); */
		}

		/* tscpu_dprintk("temperature_to_raw_abb format_1=%d, format_2=%d, format_3=%d, format_4=%d\n",
				format_1, format_2, format_3, format_4); */
	} else {		/* O_SLOPE is Negative. */
		format_1 = t_curr - (g_degc_cali * 1000 / 2);
		format_2 = format_1 * (165 - g_o_slope) * 18 / 15;
		format_2 = format_2 - 2 * format_2;

		for (i = 0; i < THERMAL_SENSOR_NUM; i++) {
			format_3[i] = format_2 / 1000 + g_x_roomt[i] * 10;
			format_4[i] = (format_3[i] * 4096 / 10000 * g_gain) / 100000 + g_oe;
			/* tscpu_dprintk("[Temperature_to_raw_roomt][roomt%d] format_1=%d,
format_2=%d, format_3=%d, format_4=%d\n",
i, format_1, format_2, format_3[i], format_4[i]); */
		}

		/* tscpu_dprintk("temperature_to_raw_abb format_1=%d, format_2=%d, format_3=%d, format_4=%d\n",
				format_1, format_2, format_3, format_4); */
	}


	temp = 0;
	for (i = 0; i < THERMAL_SENSOR_NUM; i++) {
		if (temp < format_4[i]) {
			temp = format_4[i];
			index = i;
		}
	}

	/* tscpu_dprintk("[Temperature_to_raw_roomt] temperature=%d, raw[%d]=%d", ret, index, format_4[index]); */
	return format_4[index];

}
#endif


static S32 raw_to_temperature_roomt(U32 ret, enum thermal_sensor_name ts_name)
{
	S32 t_current = 0;
	S32 y_curr = ret;
	S32 format_1 = 0;
	S32 format_2 = 0;
	S32 format_3 = 0;
	S32 format_4 = 0;
	S32 xtoomt = 0;


	xtoomt = g_x_roomt[ts_name];

	/* tscpu_dprintk("raw_to_temperature_room,ts_num=%d,xtoomt=%d\n",ts_name,xtoomt); */

	if (ret == 0)
		return 0;

	format_1 = ((g_degc_cali * 10) >> 1);
	format_2 = (y_curr - g_oe);

	format_3 = (((((format_2) * 10000) >> 12) * 10000) / g_gain) - xtoomt;
	format_3 = format_3 * 15 / 18;


	if (g_o_slope_sign == 0)
		format_4 = ((format_3 * 100) / (165 + g_o_slope));	/* uint = 0.1 deg */
	else
		format_4 = ((format_3 * 100) / (165 - g_o_slope));	/* uint = 0.1 deg */

	format_4 = format_4 - (format_4 << 1);


	t_current = format_1 + format_4;	/* uint = 0.1 deg */

	/* tscpu_dprintk("raw_to_temperature_room,t_current=%d\n",t_current); */
	return t_current;
}



static void thermal_calibration(void)
{
	if (g_adc_cali_en_t == 0)
		tscpu_printk("#####  Not Calibration  ######\n");

	/* tscpu_dprintk("thermal_calibration\n"); */
	thermal_cal_prepare_2(0);
}




int get_immediate_abb_temp_wrap(void)
{
	int curr_temp = 0;
	/* curr_temp = ABB_TS_ABB_T; */

	tscpu_dprintk("get_immediate_abb_temp_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}


/*
Bank0 : CPU (TS_MCU1,TS_MCU2)        (TS3, TS4)
Bank1 : GPU (TS_MCU3)                (TS5)
Bank2 : SOC (TS_MCU4,TS_MCU2,TS_MCU3)(TS1, TS4, TS5)
 */
int get_immediate_cpu_wrap(void)
{
	int curr_temp;

	curr_temp = MAX(CPU_TS_MCU1_T, CPU_TS_MCU2_T);

	tscpu_dprintk("get_immediate_cpu_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}

int get_immediate_gpu_wrap(void)
{
	int curr_temp;

	curr_temp = GPU_TS_MCU2_T;

	tscpu_dprintk("get_immediate_gpu_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}

int get_immediate_soc_wrap(void)
{
	int curr_temp;

	curr_temp = MAX(SOC_TS_MCU1_T, SOC_TS_MCU2_T);
	curr_temp = MAX(SOC_TS_MCU3_T, curr_temp);

	tscpu_dprintk("get_immediate_soc_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}


/*
Bank0 : CPU (TS_MCU1,TS_MCU2)        (TS3, TS4)
Bank1 : GPU (TS_MCU3)                (TS5)
Bank2 : SOC (TS_MCU4,TS_MCU2,TS_MCU3)(TS1, TS4, TS5)
 */


int get_immediate_ts1_wrap(void)
{
	int curr_temp;

	curr_temp = CPU_TS_MCU1_T;
	tscpu_dprintk("get_immediate_ts1_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}

int get_immediate_ts2_wrap(void)
{
	int curr_temp;

	curr_temp = GPU_TS_MCU2_T;
	tscpu_dprintk("get_immediate_ts2_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}

int get_immediate_ts3_wrap(void)
{
	int curr_temp;

	curr_temp = SOC_TS_MCU3_T;
	tscpu_dprintk("get_immediate_ts3_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}

int get_immediate_ts4_wrap(void)
{
	int curr_temp;

	curr_temp = MAX(CPU_TS_MCU2_T, SOC_TS_MCU2_T);
	tscpu_dprintk("get_immediate_ts4_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}

int get_immediate_ts5_wrap(void)
{
	int curr_temp;

	curr_temp = MAX(GPU_TS_MCU2_T, SOC_TS_MCU3_T);
	tscpu_dprintk("get_immediate_ts5_wrap curr_temp=%d\n", curr_temp);

	return curr_temp;
}

static int read_tc_raw_and_temp(u32 *tempmsr_name, enum thermal_sensor_name ts_name,
		int *ts_raw)
{
	int temp = 0, raw = 0;


	/*tscpu_dprintk("read_tc_raw_temp,tempmsr_name=0x%x,ts_num=%d\n",tempmsr_name,ts_name); */

	raw = (tempmsr_name != 0) ? (DRV_Reg32((tempmsr_name)) & 0x0fff) : 0;
	temp = (tempmsr_name != 0) ? raw_to_temperature_roomt(raw, ts_name) : 0;

	*ts_raw = raw;
	/*tscpu_dprintk("read_tc_raw_temp,ts_raw=%d,temp=%d\n",*ts_raw,temp*100); */

	return temp * 100;
}


/*
Bank0 : CPU (TS_MCU1)
Bank1 : GPU (TS_MCU2)
Bank2 : SOC (TS_MCU3)
 */

static void read_each_bank_TS(enum thermal_bank_name bank_num)
{

	/* tscpu_dprintk("read_each_bank_TS,bank_num=%d\n",bank_num); */

	switch (bank_num) {
	case THERMAL_BANK0:
			/* Bank0 : CPU (TS_MCU1) */
			CPU_TS_MCU1_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR0, THERMAL_SENSOR1, &CPU_TS_MCU1_R);
			CPU_TS_MCU2_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR1, THERMAL_SENSOR2, &CPU_TS_MCU2_R);
			break;
	case THERMAL_BANK1:
			/* Bank1 : GPU (TS_MCU2) */
			GPU_TS_MCU1_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR0, THERMAL_SENSOR1, &GPU_TS_MCU1_R);
			GPU_TS_MCU2_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR1, THERMAL_SENSOR2, &GPU_TS_MCU2_R);
			break;
	case THERMAL_BANK2:
			/* Bank2 : SOC (TS_MCU3) */
			SOC_TS_MCU1_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR0, THERMAL_SENSOR1, &SOC_TS_MCU1_R);
			SOC_TS_MCU2_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR1, THERMAL_SENSOR2, &SOC_TS_MCU2_R);
			SOC_TS_MCU3_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR2, THERMAL_SENSOR3, &SOC_TS_MCU3_R);
			/* ABB_TS_ABB_T  = read_tc_raw_and_temp((volatile u32 *)TEMPMSR3,
				THERMAL_SENSORABB,&ABB_TS_ABB_R); */

			break;
	default:
			/* Bank0 : CPU (TS_MCU1,TS_MCU2) */
			CPU_TS_MCU1_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR0, THERMAL_SENSOR1, &CPU_TS_MCU1_R);
			CPU_TS_MCU2_T =
				read_tc_raw_and_temp((u32 *)TEMPMSR1, THERMAL_SENSOR2, &CPU_TS_MCU2_R);
			break;
	}

}


static void read_all_bank_temperature(void)
{
	int i = 0;
	unsigned long flags;
	/* tscpu_dprintk("read_all_bank_temperature\n"); */


	mt_ptp_lock(&flags);

	for (i = 0; i < THERMAL_BANK_NUM; i++) {
		mtktscpu_switch_bank(i);
		read_each_bank_TS(i);
	}

	mt_ptp_unlock(&flags);

}


static int tscpu_get_temp(struct thermal_zone_device *thermal, unsigned long *t)
{
#if MTK_TS_CPU_SW_FILTER == 1
	int ret = 0;
	int curr_temp;

	int temp_temp;
	int bank0_T;
	int bank1_T;
	int bank2_T;

	static int last_cpu_real_temp;

	/*
Bank0 : CPU (TS_MCU1,TS_MCU2)        (TS3, TS4)
Bank1 : GPU (TS_MCU3)                (TS5)
Bank2 : SOC (TS_MCU4,TS_MCU2,TS_MCU3)(TS1, TS4, TS5)
	 */

	bank0_T = MAX(CPU_TS_MCU1_T, CPU_TS_MCU2_T);
	bank1_T = GPU_TS_MCU2_T;
	bank2_T = MAX(SOC_TS_MCU1_T, SOC_TS_MCU2_T);
	bank2_T = MAX(bank2_T, SOC_TS_MCU3_T);

	curr_temp = MAX(bank0_T, bank1_T);
	curr_temp = MAX(curr_temp, bank2_T);



	tscpu_dprintk("\n tscpu_update_tempinfo, T=%d,%d,%d,%d,%d,%d\n", CPU_TS_MCU1_T,
			CPU_TS_MCU2_T, GPU_TS_MCU2_T, SOC_TS_MCU1_T, SOC_TS_MCU2_T, SOC_TS_MCU3_T);



	if ((curr_temp > (trip_temp[0] - 15000)) || (curr_temp < -30000) || (curr_temp > 85000))
		tscpu_dprintk("CPU T=%d\n", curr_temp);

	temp_temp = curr_temp;
	/* not resumed from suspensio... */
	if (curr_temp != 0) {
		/* invalid range */
		if ((curr_temp > 150000) || (curr_temp < -20000)) {
			tscpu_dprintk("CPU temp invalid=%d\n", curr_temp);
			temp_temp = 50000;
			ret = -1;
		} else if (last_cpu_real_temp != 0) {
			/* delta 40C, invalid change */
			if ((curr_temp - last_cpu_real_temp > 40000)
					|| (last_cpu_real_temp - curr_temp > 40000)) {
				tscpu_dprintk("CPU temp float hugely temp=%d, lasttemp=%d\n",
						curr_temp, last_cpu_real_temp);
				/* tscpu_dprintk("RAW_TS2 = %d,RAW_TS3 = %d,RAW_TS4 = %d\n",RAW_TS2,RAW_TS3,RAW_TS4); */
				temp_temp = 50000;
				ret = -1;
			}
		}
	}

	last_cpu_real_temp = curr_temp;
	curr_temp = temp_temp;
	/* tscpu_dprintk("TS2 = %d,TS3 = %d,TS4 = %d\n", Temp_TS2,Temp_TS3,Temp_TS4); */
#else
	int ret = 0;
	int curr_temp;

	curr_temp = get_immediate_temp1();

	tscpu_dprintk("tscpu_get_temp CPU T1=%d\n", curr_temp);

	if ((curr_temp > (trip_temp[0] - 15000)) || (curr_temp < -30000))
		tscpu_dprintk("[Power/CPU_Thermal] CPU T=%d\n", curr_temp);
#endif

	read_curr_temp = curr_temp;
	*t = (unsigned long)curr_temp;

#if MTKTSCPU_FAST_POLLING
	cur_fp_factor = next_fp_factor;

	if (curr_temp >= fast_polling_trip_temp) {
		next_fp_factor = fast_polling_factor;
		/* it means next timeout will be in interval/fast_polling_factor */
		thermal->polling_delay = interval / fast_polling_factor;
	} else {
		next_fp_factor = 1;
		thermal->polling_delay = interval;
	}
#endif

	/* for low power */
	if ((int)*t >= polling_trip_temp1)
		;
	else if ((int)*t < polling_trip_temp2)
		thermal->polling_delay = interval * polling_factor2;
	else
		thermal->polling_delay = interval * polling_factor1;

#if CPT_ADAPTIVE_AP_COOLER
	g_prev_temp = g_curr_temp;
	g_curr_temp = curr_temp;
#endif

#if THERMAL_GPIO_OUT_TOGGLE
	/*for output signal monitor */
	tscpu_set_GPIO_toggle_for_monitor();
#endif

	g_max_temp = curr_temp;
	g_temp = curr_temp;

	tscpu_dprintk("tscpu_get_temp, current temp =%d\n", curr_temp);
	return ret;
}

int tscpu_get_cpu_current_temperature(void)
{
	return g_temp;
}
EXPORT_SYMBOL(tscpu_get_cpu_current_temperature);

int tscpu_get_temp_by_bank(enum thermal_bank_name ts_bank)
{
	int bank_T = 0;

	/* tscpu_dprintk("tscpu_get_temp_by_bank,bank=%d\n",ts_bank); */


	/*
Bank0 : CPU (TS_MCU1,TS_MCU2)        (TS3, TS4)
Bank1 : GPU (TS_MCU3)                (TS5)
Bank2 : SOC (TS_MCU4,TS_MCU2,TS_MCU3)(TS1, TS4, TS5)
	 */
	if (ts_bank == THERMAL_BANK0) {
		bank_T = MAX(CPU_TS_MCU1_T, CPU_TS_MCU2_T);
	} else if (ts_bank == THERMAL_BANK1) {
		bank_T = GPU_TS_MCU2_T;
	} else if (ts_bank == THERMAL_BANK2) {
		bank_T = MAX(SOC_TS_MCU1_T, SOC_TS_MCU2_T);
		bank_T = MAX(bank_T, SOC_TS_MCU3_T);
	}
#if 0
	tscpu_dprintk("\n\n");
	tscpu_dprintk("Bank 0 : CPU  (TS_MCU1 = %d,TS_MCU2 = %d)\n", CPU_TS_MCU1_T,	CPU_TS_MCU2_T);
	tscpu_dprintk("Bank 1 : GPU  (TS_MCU3 = %d)\n", GPU_TS_MCU3_T);
	tscpu_dprintk("Bank 2 : SOC  (TS_MCU4 = %d,TS_MCU2 = %d,TS_MCU3 = %d)\n", SOC_TS_MCU4_T,
			SOC_TS_MCU2_T, SOC_TS_MCU3_T);
#endif

	return bank_T;
}


static int tscpu_bind(struct thermal_zone_device *thermal, struct thermal_cooling_device *cdev)
{
	int table_val = 0;

	if (!strcmp(cdev->type, g_bind0)) {
		table_val = 0;

		tscpu_config_all_tc_hw_protect(trip_temp[0], tc_mid_trip);


		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind1)) {
		table_val = 1;
		/* only when a valid cooler is tried to bind here, we set tc_mid_trip to trip_temp[1]; */
		tc_mid_trip = trip_temp[1];

		tscpu_config_all_tc_hw_protect(trip_temp[0], tc_mid_trip);


		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind2)) {
		table_val = 2;
		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind3)) {
		table_val = 3;
		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind4)) {
		table_val = 4;
		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind5)) {
		table_val = 5;
		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind6)) {
		table_val = 6;
		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind7)) {
		table_val = 7;
		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind8)) {
		table_val = 8;
		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind9)) {
		table_val = 9;
		/* tscpu_dprintk("tscpu_bind %s\n", cdev->type); */
	} else {
		return 0;
	}

	if (mtk_thermal_zone_bind_cooling_device(thermal, table_val, cdev)) {
		tscpu_printk("tscpu_bind error binding cooling dev\n");
		return -EINVAL;
	}
	tscpu_dprintk("tscpu_bind binding OK, %d\n", table_val);

	return 0;
}

static int tscpu_unbind(struct thermal_zone_device *thermal, struct thermal_cooling_device *cdev)
{
	int table_val = 0;

	if (!strcmp(cdev->type, g_bind0)) {
		table_val = 0;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind1)) {
		table_val = 1;
		/* only when a valid cooler is tried to bind here, we set tc_mid_trip to trip_temp[1]; */
		tc_mid_trip = -275000;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind2)) {
		table_val = 2;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind3)) {
		table_val = 3;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind4)) {
		table_val = 4;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind5)) {
		table_val = 5;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind6)) {
		table_val = 6;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind7)) {
		table_val = 7;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind8)) {
		table_val = 8;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else if (!strcmp(cdev->type, g_bind9)) {
		table_val = 9;
		/* tscpu_dprintk("tscpu_unbind %s\n", cdev->type); */
	} else
		return 0;


	if (thermal_zone_unbind_cooling_device(thermal, table_val, cdev)) {
		tscpu_printk("tscpu_unbind error unbinding cooling dev\n");
		return -EINVAL;
	}
	tscpu_dprintk("tscpu_unbind unbinding OK\n");

	return 0;
}

static int tscpu_get_mode(struct thermal_zone_device *thermal, enum thermal_device_mode *mode)
{
	*mode = (kernelmode) ? THERMAL_DEVICE_ENABLED : THERMAL_DEVICE_DISABLED;
	return 0;
}

static int tscpu_set_mode(struct thermal_zone_device *thermal, enum thermal_device_mode mode)
{
	kernelmode = mode;
	return 0;
}

static int tscpu_get_trip_type(struct thermal_zone_device *thermal, int trip,
		enum thermal_trip_type *type)
{
	*type = g_THERMAL_TRIP[trip];
	return 0;
}

static int tscpu_get_trip_temp(struct thermal_zone_device *thermal, int trip, unsigned long *temp)
{
	*temp = trip_temp[trip];
	return 0;
}

static int tscpu_set_trip_temp(struct thermal_zone_device *thermal,
				  int trip,
				  unsigned long t)
{
	trip_temp[trip] = t;
	return 0;
}

static int tscpu_get_crit_temp(struct thermal_zone_device *thermal, unsigned long *temperature)
{
	*temperature = MTKTSCPU_TEMP_CRIT;
	return 0;
}

#define PREFIX "thermaltscpu:def"
static int tscpu_thermal_notify(struct thermal_zone_device *thermal,
				int trip, enum thermal_trip_type type)
{
	if (type == THERMAL_TRIP_CRITICAL) {
		pr_err("%s: thermal_shutdown notify\n", __func__);
		last_kmsg_thermal_shutdown();
		pr_err("%s: thermal_shutdown notify end\n", __func__);
	}
	return 0;
}

/* bind callback functions to thermalzone */
static struct thermal_zone_device_ops mtktscpu_dev_ops = {
	.bind = tscpu_bind,
	.unbind = tscpu_unbind,
	.get_temp = tscpu_get_temp,
	.get_mode = tscpu_get_mode,
	.set_mode = tscpu_set_mode,
	.get_trip_type = tscpu_get_trip_type,
	.get_trip_temp = tscpu_get_trip_temp,
	.set_trip_temp = tscpu_set_trip_temp,
	.get_crit_temp = tscpu_get_crit_temp,
	.notify = tscpu_thermal_notify,
};

static int previous_step = -1;

/*
   static int step0_mask[11] = {1,1,1,1,1,1,1,1,1,1,1};
   static int step1_mask[11] = {0,1,1,1,1,1,1,1,1,1,1};
   static int step2_mask[11] = {0,0,1,1,1,1,1,1,1,1,1};
   static int step3_mask[11] = {0,0,0,1,1,1,1,1,1,1,1};
   static int step4_mask[11] = {0,0,0,0,1,1,1,1,1,1,1};
   static int step5_mask[11] = {0,0,0,0,0,1,1,1,1,1,1};
   static int step6_mask[11] = {0,0,0,0,0,0,1,1,1,1,1};
   static int step7_mask[11] = {0,0,0,0,0,0,0,1,1,1,1};
   static int step8_mask[11] = {0,0,0,0,0,0,0,0,1,1,1};
   static int step9_mask[11] = {0,0,0,0,0,0,0,0,0,1,1};
   static int step10_mask[11]= {0,0,0,0,0,0,0,0,0,0,1};
 */
static int tscpu_set_power_consumption_state(void)
{
	int i = 0;
	int power = 0;

	tscpu_dprintk("tscpu_set_power_consumption_state Num_of_OPP=%d\n", Num_of_OPP);

	/* in 92, Num_of_OPP=34 */
	for (i = 0; i < Num_of_OPP; i++) {
		if (1 == cl_dev_state[i]) {
			if (i != previous_step) {
				tscpu_printk("previous_opp=%d, now_opp=%d\n", previous_step, i);
				previous_step = i;
				mtktscpu_limited_dmips = tscpu_cpu_dmips[previous_step];
				if (Num_of_GPU_OPP == 3) {
					power =
						(i * 100 + 700) - mtk_gpu_power[Num_of_GPU_OPP -
						1].gpufreq_power;
					set_static_cpu_power_limit(power);
					set_static_gpu_power_limit(mtk_gpu_power
							[Num_of_GPU_OPP -
							1].gpufreq_power);
					tscpu_dprintk
						("Num_of_GPU_OPP=%d, gpufreq_power=%d, power=%d\n",
						 Num_of_GPU_OPP,
						 mtk_gpu_power[Num_of_GPU_OPP - 1].gpufreq_power,
						 power);
				} else if (Num_of_GPU_OPP == 2) {
					power = (i * 100 + 700) - mtk_gpu_power[1].gpufreq_power;
					set_static_cpu_power_limit(power);
					set_static_gpu_power_limit(mtk_gpu_power[1].gpufreq_power);
					tscpu_dprintk
						("Num_of_GPU_OPP=%d, gpufreq_power=%d, power=%d\n",
						 Num_of_GPU_OPP, mtk_gpu_power[1].gpufreq_power, power);
				} else if (Num_of_GPU_OPP == 1) {
#if 0
					/* 653mW,GPU 500Mhz,1V(preloader default) */
					/* 1016mW,GPU 700Mhz,1.1V */
					power = (i * 100 + 700) - 653;
#else
					power = (i * 100 + 700) - mtk_gpu_power[0].gpufreq_power;
#endif
					set_static_cpu_power_limit(power);
					tscpu_dprintk
						("Num_of_GPU_OPP=%d, gpufreq_power=%d, power=%d\n",
						 Num_of_GPU_OPP, mtk_gpu_power[0].gpufreq_power, power);
				} else {	/* TODO: fix this, temp solution, this project has over 5 GPU OPP... */
					power = (i * 100 + 700);
					set_static_cpu_power_limit(power);
					tscpu_dprintk
						("Num_of_GPU_OPP=%d, gpufreq_power=%d, power=%d\n",
						 Num_of_GPU_OPP, mtk_gpu_power[0].gpufreq_power, power);
				}
			}
			break;
		}
	}

	/* If temp drop to our expect value, we need to restore initial cpu freq setting */
	if (i == Num_of_OPP) {
		if (previous_step != -1) {
			previous_step = -1;
			mtktscpu_limited_dmips = tscpu_cpu_dmips[CPU_COOLER_NUM - 1];	/* highest dmips */
			tscpu_dprintk("Free all static thermal limit, previous_opp=%d\n",
					previous_step);
			set_static_cpu_power_limit(0);
			set_static_gpu_power_limit(0);
		}
	}
	return 0;
}

#if CPT_ADAPTIVE_AP_COOLER

extern unsigned long (*mtk_thermal_get_gpu_loading_fp)(void);

static int GPU_L_H_TRIP = 80, GPU_L_L_TRIP = 40;

static int TARGET_TJ = 65000;
static int TARGET_TJ_HIGH = 66000;
static int TARGET_TJ_LOW = 64000;
static int PACKAGE_THETA_JA_RISE = 10;
static int PACKAGE_THETA_JA_FALL = 10;
static int MINIMUM_CPU_POWER = 500;
static int MAXIMUM_CPU_POWER = 1240;
static int MINIMUM_GPU_POWER = 676;
static int MAXIMUM_GPU_POWER = 676;
static int MINIMUM_TOTAL_POWER = 500 + 676;
static int MAXIMUM_TOTAL_POWER = 1240 + 676;
static int FIRST_STEP_TOTAL_POWER_BUDGET = 1750;

/* 1. MINIMUM_BUDGET_CHANGE = 0 ==> thermal equilibrium maybe at higher than TARGET_TJ_HIGH */
/* 2. Set MINIMUM_BUDGET_CHANGE > 0 if to keep Tj at TARGET_TJ */
static int MINIMUM_BUDGET_CHANGE = 50;

static int P_adaptive(int total_power, unsigned int gpu_loading)
{
	/* Here the gpu_power is the gpu power limit for the next interval,
	not exactly what gpu power selected by GPU DVFS */
	/* But the ground rule is real gpu power should always under gpu_power for the same time interval */
	static int cpu_power = 0, gpu_power;
	static int last_cpu_power = 0, last_gpu_power;

	last_cpu_power = cpu_power;
	last_gpu_power = gpu_power;

	if (total_power == 0) {
		cpu_power = gpu_power = 0;
#if THERMAL_HEADROOM
		if (thp_max_cpu_power != 0)
			set_adaptive_cpu_power_limit((unsigned int)
					MAX(thp_max_cpu_power, MINIMUM_CPU_POWER));
		else
			set_adaptive_cpu_power_limit(0);
#else
		set_adaptive_cpu_power_limit(0);
#endif
		set_adaptive_gpu_power_limit(0);
		return 0;
	}

	if (total_power <= MINIMUM_TOTAL_POWER) {
		cpu_power = MINIMUM_CPU_POWER;
		gpu_power = MINIMUM_GPU_POWER;
	} else if (total_power >= MAXIMUM_TOTAL_POWER) {
		cpu_power = MAXIMUM_CPU_POWER;
		gpu_power = MAXIMUM_GPU_POWER;
	} else {
		int max_allowed_gpu_power =
			MIN((total_power - MINIMUM_CPU_POWER), MAXIMUM_GPU_POWER);
		int highest_possible_gpu_power = -1;
		/* int highest_possible_gpu_power_idx = 0; */
		int i = 0;
		unsigned int cur_gpu_freq = mt_gpufreq_get_cur_freq();
		/* int cur_idx = 0; */
		unsigned int cur_gpu_power = 0;
		unsigned int next_lower_gpu_power = 0;

		/* get GPU highest possible power and index and current power and index and next lower power */
		for (; i < Num_of_GPU_OPP; i++) {
			if ((mtk_gpu_power[i].gpufreq_power <= max_allowed_gpu_power) &&
					(-1 == highest_possible_gpu_power)) {
				highest_possible_gpu_power = mtk_gpu_power[i].gpufreq_power;
				/* highest_possible_gpu_power_idx = i; */
			}

			if (mtk_gpu_power[i].gpufreq_khz == cur_gpu_freq) {
				next_lower_gpu_power = cur_gpu_power =
					mtk_gpu_power[i].gpufreq_power;
				/* cur_idx = i; */

				if ((i != Num_of_GPU_OPP - 1)
						&& (mtk_gpu_power[i + 1].gpufreq_power >= MINIMUM_GPU_POWER)) {
					next_lower_gpu_power = mtk_gpu_power[i + 1].gpufreq_power;
				}
			}
		}

		/* decide GPU power limit by loading */
		if (gpu_loading > GPU_L_H_TRIP) {
			gpu_power = highest_possible_gpu_power;
		} else if (gpu_loading <= GPU_L_L_TRIP) {
			gpu_power = MIN(next_lower_gpu_power, highest_possible_gpu_power);
			gpu_power = MAX(gpu_power, MINIMUM_GPU_POWER);
		} else {
			gpu_power = MIN(highest_possible_gpu_power, cur_gpu_power);
		}

		cpu_power = MIN((total_power - gpu_power), MAXIMUM_CPU_POWER);
	}

	if (cpu_power != last_cpu_power)
		set_adaptive_cpu_power_limit(cpu_power);
	if (gpu_power != last_gpu_power)
		set_adaptive_gpu_power_limit(gpu_power);

	tscpu_dprintk("%s cpu %d, gpu %d\n", __func__, cpu_power, gpu_power);

	return 0;
}

static int _adaptive_power(long prev_temp, long curr_temp, unsigned int gpu_loading)
{
	static int triggered = 0, total_power;
	int delta_power = 0;

	if (cl_dev_adp_cpu_state_active == 1) {

		/* Check if it is triggered */
		if (!triggered) {
			if (curr_temp < TARGET_TJ)
				return 0;

				triggered = 1;
				switch (mtktscpu_atm) {
				case 1:	/* FTL ATM v2 */
				case 2:	/* CPU_GPU_Weight ATM v2 */
#if MTKTSCPU_FAST_POLLING
						total_power =
							FIRST_STEP_TOTAL_POWER_BUDGET -
							((curr_temp - TARGET_TJ) * tt_ratio_high_rise +
							 (curr_temp -
							  prev_temp) * tp_ratio_high_rise) /
							(PACKAGE_THETA_JA_RISE * cur_fp_factor);
#else
						total_power =
							FIRST_STEP_TOTAL_POWER_BUDGET -
							((curr_temp - TARGET_TJ) * tt_ratio_high_rise +
							 (curr_temp -
							  prev_temp) * tp_ratio_high_rise) /
							PACKAGE_THETA_JA_RISE;
#endif
						break;
				case 0:
				default:	/* ATM v1 */
						total_power = FIRST_STEP_TOTAL_POWER_BUDGET;
				}
				tscpu_dprintk("start %s Tp %d, Tc %d, Pt %d\n", __func__, (int)prev_temp,
						(int)curr_temp, total_power);
				return P_adaptive(total_power, gpu_loading);
		}

		/* Adjust total power budget if necessary */
		switch (mtktscpu_atm) {
		case 1:	/* FTL ATM v2 */
		case 2:	/* CPU_GPU_Weight ATM v2 */
				if ((curr_temp >= TARGET_TJ_HIGH) && (curr_temp > prev_temp)) {
#if MTKTSCPU_FAST_POLLING
					total_power -=
						MAX(((curr_temp - TARGET_TJ) * tt_ratio_high_rise +
									(curr_temp -
									 prev_temp) * tp_ratio_high_rise) /
								(PACKAGE_THETA_JA_RISE * cur_fp_factor),
								MINIMUM_BUDGET_CHANGE);
#else
					total_power -=
						MAX(((curr_temp - TARGET_TJ) * tt_ratio_high_rise +
(curr_temp - prev_temp) * tp_ratio_high_rise) / PACKAGE_THETA_JA_RISE, MINIMUM_BUDGET_CHANGE);
#endif
				} else if ((curr_temp >= TARGET_TJ_HIGH) && (curr_temp <= prev_temp)) {
#if MTKTSCPU_FAST_POLLING
					total_power -=
						MAX(((curr_temp - TARGET_TJ) * tt_ratio_high_fall -
									(prev_temp -
									 curr_temp) * tp_ratio_high_fall) /
								(PACKAGE_THETA_JA_FALL * cur_fp_factor),
								MINIMUM_BUDGET_CHANGE);
#else
					total_power -=
						MAX(((curr_temp - TARGET_TJ) * tt_ratio_high_fall -
(prev_temp - curr_temp) * tp_ratio_high_fall) / PACKAGE_THETA_JA_FALL,
MINIMUM_BUDGET_CHANGE);
#endif
				} else if ((curr_temp <= TARGET_TJ_LOW) && (curr_temp > prev_temp)) {
#if MTKTSCPU_FAST_POLLING
					total_power +=
						MAX(((TARGET_TJ - curr_temp) * tt_ratio_low_rise -
(curr_temp - prev_temp) * tp_ratio_low_rise) / (PACKAGE_THETA_JA_RISE * cur_fp_factor),
MINIMUM_BUDGET_CHANGE);
#else
					total_power += MAX(MINIMUM_BUDGET_CHANGE,
						((TARGET_TJ - curr_temp) * tt_ratio_low_rise -
						(curr_temp - prev_temp) * tp_ratio_low_rise) / PACKAGE_THETA_JA_RISE);
#endif
				} else if ((curr_temp <= TARGET_TJ_LOW) && (curr_temp <= prev_temp)) {
#if MTKTSCPU_FAST_POLLING
					total_power += MAX(MINIMUM_BUDGET_CHANGE,
					((TARGET_TJ - curr_temp) * tt_ratio_low_fall +
(prev_temp - curr_temp) * tp_ratio_low_fall) / (PACKAGE_THETA_JA_FALL * cur_fp_factor));
#else
					total_power += MAX(MINIMUM_BUDGET_CHANGE,
					((TARGET_TJ - curr_temp) * tt_ratio_low_fall +
						(prev_temp - curr_temp) * tp_ratio_low_fall) / PACKAGE_THETA_JA_FALL);
#endif
				}

				total_power =
					(total_power > MINIMUM_TOTAL_POWER) ? total_power : MINIMUM_TOTAL_POWER;
				total_power =
					(total_power < MAXIMUM_TOTAL_POWER) ? total_power : MAXIMUM_TOTAL_POWER;
				break;

		case 0:
		default:	/* ATM v1 */
				if ((curr_temp > TARGET_TJ_HIGH) && (curr_temp >= prev_temp)) {
#if MTKTSCPU_FAST_POLLING
					delta_power =
						(curr_temp -
						 prev_temp) / (PACKAGE_THETA_JA_RISE * cur_fp_factor);
#else
					delta_power = (curr_temp - prev_temp) / PACKAGE_THETA_JA_RISE;
#endif
					if (prev_temp > TARGET_TJ_HIGH) {
						delta_power =
							(delta_power >
							 MINIMUM_BUDGET_CHANGE) ? delta_power :
							MINIMUM_BUDGET_CHANGE;
					}
					total_power -= delta_power;
					total_power =
						(total_power >
						 MINIMUM_TOTAL_POWER) ? total_power : MINIMUM_TOTAL_POWER;
				}

				if ((curr_temp < TARGET_TJ_LOW) && (curr_temp <= prev_temp)) {
#if MTKTSCPU_FAST_POLLING
					delta_power =
						(prev_temp -
						 curr_temp) / (PACKAGE_THETA_JA_FALL * cur_fp_factor);
#else
					delta_power = (prev_temp - curr_temp) / PACKAGE_THETA_JA_FALL;
#endif
					if (prev_temp < TARGET_TJ_LOW) {
						delta_power =
							(delta_power >
							 MINIMUM_BUDGET_CHANGE) ? delta_power :
							MINIMUM_BUDGET_CHANGE;
					}
					total_power += delta_power;
					total_power =
						(total_power <
						 MAXIMUM_TOTAL_POWER) ? total_power : MAXIMUM_TOTAL_POWER;
				}
				break;
		}
		tscpu_dprintk("get %s Tp %d, Tc %d, Pt %d\n", __func__, (int)prev_temp, (int)curr_temp,
				total_power);
		return P_adaptive(total_power, gpu_loading);
	}
	if (triggered) {
			triggered = 0;
			tscpu_dprintk("stop %s Tp %d, Tc %d, Pt %d\n", __func__, (int)prev_temp,
					(int)curr_temp, total_power);
			return P_adaptive(0, 0);
	}

#if THERMAL_HEADROOM
		if (thp_max_cpu_power != 0)
			set_adaptive_cpu_power_limit((unsigned int)
					MAX(thp_max_cpu_power, MINIMUM_CPU_POWER));
		else
				set_adaptive_cpu_power_limit(0);
#endif

	return 0;
}

static int decide_ttj(void)
{
	int i = 0;
	int active_cooler_id = -1;
	int ret = 117000;	/* highest allowable TJ */
	int temp_cl_dev_adp_cpu_state_active = 0;

	for (; i < MAX_CPT_ADAPTIVE_COOLERS; i++) {
		if (cl_dev_adp_cpu_state[i]) {
			ret = MIN(ret, TARGET_TJS[i]);
			temp_cl_dev_adp_cpu_state_active = 1;

			if (ret == TARGET_TJS[i])
				active_cooler_id = i;
		}
	}
	cl_dev_adp_cpu_state_active = temp_cl_dev_adp_cpu_state_active;
	TARGET_TJ = ret;
	TARGET_TJ_HIGH = TARGET_TJ + 1000;
	TARGET_TJ_LOW = TARGET_TJ - 1000;

	if (0 <= active_cooler_id && MAX_CPT_ADAPTIVE_COOLERS > active_cooler_id) {
		PACKAGE_THETA_JA_RISE = PACKAGE_THETA_JA_RISES[active_cooler_id];
		PACKAGE_THETA_JA_FALL = PACKAGE_THETA_JA_FALLS[active_cooler_id];
		MINIMUM_CPU_POWER = MINIMUM_CPU_POWERS[active_cooler_id];
		MAXIMUM_CPU_POWER = MAXIMUM_CPU_POWERS[active_cooler_id];
		MINIMUM_GPU_POWER = MINIMUM_GPU_POWERS[active_cooler_id];
		MAXIMUM_GPU_POWER = MAXIMUM_GPU_POWERS[active_cooler_id];
		MINIMUM_TOTAL_POWER = MINIMUM_CPU_POWER + MINIMUM_GPU_POWER;
		MAXIMUM_TOTAL_POWER = MAXIMUM_CPU_POWER + MAXIMUM_GPU_POWER;
		FIRST_STEP_TOTAL_POWER_BUDGET = FIRST_STEP_TOTAL_POWER_BUDGETS[active_cooler_id];
		MINIMUM_BUDGET_CHANGE = MINIMUM_BUDGET_CHANGES[active_cooler_id];
	} else {
		MINIMUM_CPU_POWER = MINIMUM_CPU_POWERS[0];
		MAXIMUM_CPU_POWER = MAXIMUM_CPU_POWERS[0];
	}

#if THERMAL_HEADROOM
	MAXIMUM_CPU_POWER -= p_Tpcb_correlation * MAX((bts_cur_temp - Tpcb_trip_point), 0) / 1000;

	/* tscpu_printk("max_cpu_pwr %d %d\n", bts_cur_temp, MAXIMUM_CPU_POWER); */
	/* TODO: use 0 as current P */
	thp_max_cpu_power = (thp_threshold_tj - read_curr_temp) * thp_p_tj_correlation / 1000 + 0;

	if (thp_max_cpu_power != 0)
		MAXIMUM_CPU_POWER = MIN(MAXIMUM_CPU_POWER, thp_max_cpu_power);

	MAXIMUM_CPU_POWER = MAX(MAXIMUM_CPU_POWER, MINIMUM_CPU_POWER);

	/* tscpu_printk("thp max_cpu_pwr %d %d\n", thp_max_cpu_power, MAXIMUM_CPU_POWER); */
#endif

	return ret;
}
#endif

static int dtm_cpu_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	/* tscpu_dprintk("dtm_cpu_get_max_state\n"); */
	*state = 1;
	return 0;
}

static int dtm_cpu_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	int i = 0;
	/* tscpu_dprintk("dtm_cpu_get_cur_state %s\n", cdev->type); */

	for (i = 0; i < Num_of_OPP; i++) {
		if (!strcmp(cdev->type, &cooler_name[i * 20]))
			*state = cl_dev_state[i];
	}
	return 0;
}

#define PREFIX "thermaltscpu:def"
static int dtm_cpu_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	int i = 0;
	tscpu_printk("[%s]: CPU change state, cooler=%s, state=%ld\n",
			__func__, cdev->type, state);
	for (i = 0; i < Num_of_OPP; i++) {
		if (!strcmp(cdev->type, &cooler_name[i * 20])) {
			cl_dev_state[i] = state;
			tscpu_set_power_consumption_state();
			break;
		}
	}
	return 0;
}

/*
 * cooling device callback functions (tscpu_cooling_sysrst_ops)
 * 1 : ON and 0 : OFF
 */
static int sysrst_cpu_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	/* tscpu_dprintk("sysrst_cpu_get_max_state\n"); */
	*state = 1;
	return 0;
}

static int sysrst_cpu_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	/* tscpu_dprintk("sysrst_cpu_get_cur_state\n"); */
	*state = cl_dev_sysrst_state;
	return 0;
}

static int sysrst_cpu_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	cl_dev_sysrst_state = state;

	if (cl_dev_sysrst_state == 1) {
		mtkts_dump_cali_info();
		tscpu_dprintk("sysrst_cpu_set_cur_state = 1\n");
		tscpu_dprintk("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
		tscpu_dprintk("*****************************************\n");
		tscpu_dprintk("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");

#ifndef CONFIG_ARM64
		kernel_restart("thermal sysrst_cpu_set_cur_state");
#else
		*(unsigned int *)0x0 = 0xdead;	/* To trigger data abort to reset the system for thermal protection. */
#endif

	}
	return 0;
}

#if CPT_ADAPTIVE_AP_COOLER
static int adp_cpu_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	/* tscpu_dprintk("adp_cpu_get_max_state\n"); */
	*state = 1;
	return 0;
}

static int adp_cpu_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	/* tscpu_dprintk("adp_cpu_get_cur_state\n"); */
	*state = cl_dev_adp_cpu_state[(cdev->type[13] - '0')];
	/* *state = cl_dev_adp_cpu_state; */
	return 0;
}

static int adp_cpu_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	int ttj = 117000;

	cl_dev_adp_cpu_state[(cdev->type[13] - '0')] = state;

	ttj = decide_ttj();	/* TODO: no exit point can be obtained in mtk_ts_cpu.c */

	/* tscpu_dprintk("adp_cpu_set_cur_state[%d] =%d, ttj=%d\n", (cdev->type[13] - '0'), state, ttj); */

	if (active_adp_cooler == (int)(cdev->type[13] - '0')) {
		/* = (NULL == mtk_thermal_get_gpu_loading_fp) ? 0 : mtk_thermal_get_gpu_loading_fp(); */
		unsigned int gpu_loading;

		if (!mtk_get_gpu_loading(&gpu_loading))
			gpu_loading = 0;
		_adaptive_power(g_prev_temp, g_curr_temp, (unsigned int)gpu_loading);
		/* _adaptive_power(g_prev_temp, g_curr_temp, (unsigned int) 0); */
	}
	return 0;
}
#endif

/* bind fan callbacks to fan device */

static struct thermal_cooling_device_ops mtktscpu_cooling_F0x2_ops = {
	.get_max_state = dtm_cpu_get_max_state,
	.get_cur_state = dtm_cpu_get_cur_state,
	.set_cur_state = dtm_cpu_set_cur_state,
};

#if CPT_ADAPTIVE_AP_COOLER
static struct thermal_cooling_device_ops mtktscpu_cooler_adp_cpu_ops = {
	.get_max_state = adp_cpu_get_max_state,
	.get_cur_state = adp_cpu_get_cur_state,
	.set_cur_state = adp_cpu_set_cur_state,
};
#endif

static struct thermal_cooling_device_ops mtktscpu_cooling_sysrst_ops = {
	.get_max_state = sysrst_cpu_get_max_state,
	.get_cur_state = sysrst_cpu_get_cur_state,
	.set_cur_state = sysrst_cpu_set_cur_state,
};

static int tscpu_read_Tj_out(struct seq_file *m, void *v)
{

	int ts_con0 = 0;

	/*      TS_CON0[19:16] = 0x8: Tj sensor Analog signal output via HW pin */
	ts_con0 = DRV_Reg32(TS_CON0);

	seq_printf(m, "TS_CON0:0x%x\n", ts_con0);


	return 0;
}


static ssize_t tscpu_write_Tj_out(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	char desc[32];
	int lv_Tj_out_flag;
	int len = 0;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;

	desc[len] = '\0';

	if (kstrtoint(desc, 10, &lv_Tj_out_flag) == 0) {
		if (lv_Tj_out_flag == 1) {
			/*      TS_CON0[19:16] = 0x8: Tj sensor Analog signal output via HW pin */
			THERMAL_WRAP_WR32(DRV_Reg32(TS_CON0) | 0x00010000, TS_CON0);
		} else {
			/*      TS_CON0[19:16] = 0x8: Tj sensor Analog signal output via HW pin */
			THERMAL_WRAP_WR32(DRV_Reg32(TS_CON0) & 0xfffeffff, TS_CON0);
		}

		tscpu_dprintk("tscpu_write_Tj_out lv_Tj_out_flag=%d\n", lv_Tj_out_flag);
		return count;
	}
	tscpu_dprintk("tscpu_write_Tj_out bad argument\n");

	return -EINVAL;
}


#if THERMAL_GPIO_OUT_TOGGLE
static int g_trigger_temp = 95000;	/* default 95 deg */
static int g_GPIO_out_enable;	/* 0:disable */
static int g_GPIO_already_set;

#define GPIO_BASE (0xF0005000)
#define GPIO118_MODE           (GPIO_BASE + 0x0770)
#define GPIO118_DIR            (GPIO_BASE + 0x0070)
#define GPIO118_DOUT           (GPIO_BASE + 0x0470)

static void tscpu_set_GPIO_toggle_for_monitor(void)
{
	int lv_GPIO118_MODE, lv_GPIO118_DIR, lv_GPIO118_DOUT;

	tscpu_dprintk("tscpu_set_GPIO_toggle_for_monitor,g_GPIO_out_enable=%d\n",
			g_GPIO_out_enable);

	if (g_GPIO_out_enable == 1) {
		if (g_max_temp > g_trigger_temp) {

			tscpu_printk("g_max_temp %d > g_trigger_temp %d\n", g_max_temp,
					g_trigger_temp);

			g_GPIO_out_enable = 0;	/* only can enter once */
			g_GPIO_already_set = 1;

			lv_GPIO118_MODE = thermal_readl(GPIO118_MODE);
			lv_GPIO118_DIR = thermal_readl(GPIO118_DIR);
			lv_GPIO118_DOUT = thermal_readl(GPIO118_DOUT);

			tscpu_printk("tscpu_set_GPIO_toggle_for_monitor:lv_GPIO118_MODE=0x%x,", lv_GPIO118_MODE);
			tscpu_printk("lv_GPIO118_DIR=0x%x,lv_GPIO118_DOUT=0x%x,\n", lv_GPIO118_DIR, lv_GPIO118_DOUT);

			/* thermal_clrl(GPIO118_MODE,0x00000E00);clear GPIO118_MODE[11:9] */
			/* thermal_setl(GPIO118_DIR, 0x00000040);set GPIO118_DIR[6]=1 */
			thermal_clrl(GPIO118_DOUT, 0x00000040);	/* set GPIO118_DOUT[6]=0 Low */
			udelay(200);
			thermal_setl(GPIO118_DOUT, 0x00000040);	/* set GPIO118_DOUT[6]=1 Hiht */
		} else {
			if (g_GPIO_already_set == 1) {
				/* restore */
				g_GPIO_already_set = 0;
				/* thermal_writel(GPIO118_MODE,lv_GPIO118_MODE); */
				/* thermal_writel(GPIO118_DIR, lv_GPIO118_DIR); */
				/* thermal_writel(GPIO118_DOUT,lv_GPIO118_DOUT); */
				thermal_clrl(GPIO118_DOUT, 0x00000040);	/* set GPIO118_DOUT[6]=0 Low */

			}
		}
	}

}

/*
   For example:
GPIO_BASE :0x10005000

GPIO118_MODE = 0 (change to GPIO mode)
0x0770	GPIO_MODE24	16		GPIO Mode Control Register 24
11	9	GPIO118_MODE	RW	Public	3'd1
"0: GPIO118 (IO)1: UTXD3 (O)2: URXD3 (I)3: MD_UTXD (O)
4: LTE_UTXD (O)5: TDD_TXD (O)6: Reserved7: DBG_MON_A_10_ (IO)"
Selects GPIO 118 mode

GPIO118_DIR =1  (output)
0x0070	GPIO_DIR8	16		GPIO Direction Control Register 8
6	6	GPIO118_DIR	RW	Public	1'b0	"0: Input1: Output"	GPIO 118 direction control

GPIO118_DOUT=1/0 (hi or low)
0x0470	GPIO_DOUT8	16		GPIO Data Output Register 8
6	6	GPIO118_DOUT	RW	Public	1'b0	"0: Output 01: Output 1"	GPIO 118 data output value

 */
static int tscpu_read_GPIO_out(struct seq_file *m, void *v)
{

	seq_printf(m, "GPIO out enable:%d, trigger temperature=%d\n", g_GPIO_out_enable,
			g_trigger_temp);

	return 0;
}


static ssize_t tscpu_write_GPIO_out(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	char desc[512];
	char TEMP[10], ENABLE[10];
	unsigned int valTEMP, valENABLE;


	int len = 0;

	int lv_GPIO118_MODE, lv_GPIO118_DIR;


	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;

	desc[len] = '\0';

	if (sscanf(desc, "%9s %d %9s %d ", TEMP, &valTEMP, ENABLE, &valENABLE) == 4) {
		/* tscpu_printk("XXXXXXXXX\n"); */

		if (!strcmp(TEMP, "TEMP")) {
			g_trigger_temp = valTEMP;
			tscpu_printk("g_trigger_temp=%d\n", valTEMP);
		} else {
			tscpu_printk("tscpu_write_GPIO_out TEMP bad argument\n");
			return -EINVAL;
		}

		if (!strcmp(ENABLE, "ENABLE")) {
			g_GPIO_out_enable = valENABLE;
			tscpu_printk("g_GPIO_out_enable=%d,g_GPIO_already_set=%d\n", valENABLE,
					g_GPIO_already_set);
		} else {
			tscpu_printk("tscpu_write_GPIO_out ENABLE bad argument\n");
			return -EINVAL;
		}

		lv_GPIO118_MODE = thermal_readl(GPIO118_MODE);
		lv_GPIO118_DIR = thermal_readl(GPIO118_DIR);
		/* clear GPIO118_MODE[11:9],GPIO118_MODE = 0 (change to GPIO mode) */
		thermal_clrl(GPIO118_MODE, 0x00000E00);
		thermal_setl(GPIO118_DIR, 0x00000040);	/* set GPIO118_DIR[6]=1,GPIO118_DIR =1  (output) */
		thermal_clrl(GPIO118_DOUT, 0x00000040);	/* set GPIO118_DOUT[6]=0 Low */
		return count;
	}
	tscpu_printk("tscpu_write_GPIO_out bad argument\n");

	return -EINVAL;
}
#endif


static int tscpu_read_opp(struct seq_file *m, void *v)
{

	unsigned int cpu_power, gpu_power;
	unsigned int gpu_loading = 0;

	cpu_power = MIN(adaptive_cpu_power_limit, static_cpu_power_limit);

	gpu_power = MIN(adaptive_gpu_power_limit, static_gpu_power_limit);

#if CPT_ADAPTIVE_AP_COOLER
	if (!mtk_get_gpu_loading(&gpu_loading))
		gpu_loading = 0;

	seq_printf(m, "%d,%d,%d,%d\n",
			(int)((cpu_power != 0x7FFFFFFF) ? cpu_power : 0),
			(int)((gpu_power != 0x7FFFFFFF) ? gpu_power : 0),
			/* ((NULL == mtk_thermal_get_gpu_loading_fp) ? 0 : mtk_thermal_get_gpu_loading_fp()), */
			(int)gpu_loading, (int)mt_gpufreq_get_cur_freq());

#else
	seq_printf(m, "%d,%d,0,%d\n",
			(int)((cpu_power != 0x7FFFFFFF) ? cpu_power : 0),
			(int)((gpu_power != 0x7FFFFFFF) ? gpu_power : 0),
			(int)mt_gpufreq_get_cur_freq());
#endif


	return 0;
}

static int tscpu_read_temperature_info(struct seq_file *m, void *v)
{


	seq_printf(m, "current temp:%d\n", read_curr_temp);
	seq_printf(m, "calefuse1:0x%x\n", calefuse1);
	seq_printf(m, "calefuse2:0x%x\n", calefuse2);
	seq_printf(m, "calefuse3:0x%x\n", calefuse3);
	seq_printf(m, "g_adc_ge_t:%d\n", g_adc_ge_t);
	seq_printf(m, "g_adc_oe_t:%d\n", g_adc_oe_t);
	seq_printf(m, "g_degc_cali:%d\n", g_degc_cali);
	seq_printf(m, "g_adc_cali_en_t:%d\n", g_adc_cali_en_t);
	seq_printf(m, "g_o_slope:%d\n", g_o_slope);
	seq_printf(m, "g_o_slope_sign:%d\n", g_o_slope_sign);
	seq_printf(m, "g_id:%d\n", g_id);
	seq_printf(m, "g_o_vtsmcu1:%d\n", g_o_vtsmcu1);
	seq_printf(m, "g_o_vtsmcu2:%d\n", g_o_vtsmcu2);
	seq_printf(m, "g_o_vtsmcu3:%d\n", g_o_vtsmcu3);
	seq_printf(m, "g_o_vtsmcu4:%d\n", g_o_vtsmcu4);
	seq_printf(m, "g_o_vtsabb :%d\n", g_o_vtsabb);

	return 0;
}

static int tscpu_talking_flag_read(struct seq_file *m, void *v)
{

	seq_printf(m, "%d\n", talking_flag);

	return 0;
}

static ssize_t tscpu_talking_flag_write(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	char desc[32];
	int lv_talking_flag;
	int len = 0;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;
	desc[len] = '\0';

	if (kstrtoint(desc, 10, &lv_talking_flag) == 0) {
		talking_flag = lv_talking_flag;
		tscpu_dprintk("tscpu_talking_flag_write talking_flag=%d\n", talking_flag);
		return count;
	}
	tscpu_dprintk("tscpu_talking_flag_write bad argument\n");

	return -EINVAL;
}

static int tscpu_set_temperature_read(struct seq_file *m, void *v)
{


	seq_printf(m, "%d\n", temperature_switch);

	return 0;
}


static ssize_t tscpu_set_temperature_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *data)
{
	char desc[32];
	int lv_tempe_switch;
	int len = 0;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;
	desc[len] = '\0';

	tscpu_dprintk("tscpu_set_temperature_write\n");

	/*if (sscanf(desc, "%d", &lv_tempe_switch) == 1) {*/
	if (kstrtoint(desc, 10, &lv_tempe_switch) == 0) {
		temperature_switch = lv_tempe_switch;

		tscpu_config_all_tc_hw_protect(temperature_switch, tc_mid_trip);

		tscpu_dprintk("tscpu_set_temperature_write temperature_switch=%d\n",
				temperature_switch);
		return count;
	}
	tscpu_printk("tscpu_set_temperature_write bad argument\n");

	return -EINVAL;
}

static int tscpu_read_log(struct seq_file *m, void *v)
{

	seq_printf(m, "[ tscpu_read_log] log = %d\n", mtktscpu_debug_log);


	return 0;
}

static int tscpu_read_cal(struct seq_file *m, void *v)
{


	/* seq_printf(m, "mtktscpu cal:\n devinfo index(16)=0x%x, devinfo index(17)=0x%x, devinfo index(18)=0x%x\n", */
	/* get_devinfo_with_index(16), get_devinfo_with_index(17), get_devinfo_with_index(18)); */


	return 0;
}

static int tscpu_read(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m,
			"[tscpu_read]%d\ntrip_0=%d %d %s\ntrip_1=%d %d %s\ntrip_2=%d %d %s\ntrip_3=%d %d %s\ntrip_4=%d %d %s\ntrip_5=%d %d %s\ntrip_6=%d %d %s\ntrip_7=%d %d %s\ntrip_8=%d %d %s\ntrip_9=%d %d %s\ninterval=%d\n",
			num_trip,
			trip_temp[0], g_THERMAL_TRIP[0], g_bind0,
			trip_temp[1], g_THERMAL_TRIP[1], g_bind1,
			trip_temp[2], g_THERMAL_TRIP[2], g_bind2,
			trip_temp[3], g_THERMAL_TRIP[3], g_bind3,
			trip_temp[4], g_THERMAL_TRIP[4], g_bind4,
			trip_temp[5], g_THERMAL_TRIP[5], g_bind5,
			trip_temp[6], g_THERMAL_TRIP[6], g_bind6,
			trip_temp[7], g_THERMAL_TRIP[7], g_bind7,
			trip_temp[8], g_THERMAL_TRIP[8], g_bind8,
			trip_temp[9], g_THERMAL_TRIP[9], g_bind9, interval);

	for (i = 0; i < Num_of_GPU_OPP; i++)
		seq_printf(m, "g %d %d %d\n", i, mtk_gpu_power[i].gpufreq_khz,
				mtk_gpu_power[i].gpufreq_power);

	for (i = 0; i < tscpu_num_opp; i++)
		seq_printf(m, "c %d %d %d %d\n", i, mtk_cpu_power[i].cpufreq_khz,
				mtk_cpu_power[i].cpufreq_ncpu, mtk_cpu_power[i].cpufreq_power);

	for (i = 0; i < CPU_COOLER_NUM; i++)
		seq_printf(m, "d %d %d\n", i, tscpu_cpu_dmips[i]);


	return 0;
}

static ssize_t tscpu_write_log(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	char desc[32];
	int log_switch;
	int len = 0;


	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;
	desc[len] = '\0';

	/*if (sscanf(desc, "%d", &log_switch) == 1)*/
	if (kstrtoint(desc, 10, &log_switch) == 0) {
		mtktscpu_debug_log = log_switch;
		return count;
	}
	tscpu_printk("mtktscpu_write_log bad argument\n");

	return -EINVAL;

}

#if CPT_ADAPTIVE_AP_COOLER

/* +ASC+ */
static int tscpu_read_atm(struct seq_file *m, void *v)
{

	seq_printf(m, "[tscpu_read_atm] ver = %d\n", mtktscpu_atm);
	seq_printf(m, "tt_ratio_high_rise = %d\n", tt_ratio_high_rise);
	seq_printf(m, "tt_ratio_high_fall = %d\n", tt_ratio_high_fall);
	seq_printf(m, "tt_ratio_low_rise = %d\n", tt_ratio_low_rise);
	seq_printf(m, "tt_ratio_low_fall = %d\n", tt_ratio_low_fall);
	seq_printf(m, "tp_ratio_high_rise = %d\n", tp_ratio_high_rise);
	seq_printf(m, "tp_ratio_high_fall = %d\n", tp_ratio_high_fall);
	seq_printf(m, "tp_ratio_low_rise = %d\n", tp_ratio_low_rise);
	seq_printf(m, "tp_ratio_low_fall = %d\n", tp_ratio_low_fall);

	return 0;
}

/* -ASC- */

/* +ASC+ */
static ssize_t tscpu_write_atm(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	char desc[128];
	int atm_ver;
	int tmp_tt_ratio_high_rise;
	int tmp_tt_ratio_high_fall;
	int tmp_tt_ratio_low_rise;
	int tmp_tt_ratio_low_fall;
	int tmp_tp_ratio_high_rise;
	int tmp_tp_ratio_high_fall;
	int tmp_tp_ratio_low_rise;
	int tmp_tp_ratio_low_fall;
	int len = 0;


	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;

	desc[len] = '\0';

	if (sscanf(desc, "%d %d %d %d %d %d %d %d %d ",
				&atm_ver, &tmp_tt_ratio_high_rise, &tmp_tt_ratio_high_fall,
				&tmp_tt_ratio_low_rise, &tmp_tt_ratio_low_fall, &tmp_tp_ratio_high_rise,
				&tmp_tp_ratio_high_fall, &tmp_tp_ratio_low_rise, &tmp_tp_ratio_low_fall) == 9)
		/* if (5 <= sscanf(desc, "%d %d %d %d %d", &log_switch, &hot, &normal, &low, &lv_offset)) */
	{
		mtktscpu_atm = atm_ver;
		tt_ratio_high_rise = tmp_tt_ratio_high_rise;
		tt_ratio_high_fall = tmp_tt_ratio_high_fall;
		tt_ratio_low_rise = tmp_tt_ratio_low_rise;
		tt_ratio_low_fall = tmp_tt_ratio_low_fall;
		tp_ratio_high_rise = tmp_tp_ratio_high_rise;
		tp_ratio_high_fall = tmp_tp_ratio_high_fall;
		tp_ratio_low_rise = tmp_tp_ratio_low_rise;
		tp_ratio_low_fall = tmp_tp_ratio_low_fall;
		return count;
	}
	tscpu_printk("mtktscpu_write_atm bad argument\n");

	return -EINVAL;

}

/* -ASC- */

static int tscpu_read_dtm_setting(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < MAX_CPT_ADAPTIVE_COOLERS; i++) {
		seq_printf(m, "%s%02d\n", adaptive_cooler_name, i);
		seq_printf(m, " first_step = %d\n", FIRST_STEP_TOTAL_POWER_BUDGETS[i]);
		seq_printf(m, " theta rise = %d\n", PACKAGE_THETA_JA_RISES[i]);
		seq_printf(m, " theta fall = %d\n", PACKAGE_THETA_JA_FALLS[i]);
		seq_printf(m, " min_budget_change = %d\n", MINIMUM_BUDGET_CHANGES[i]);
		seq_printf(m, " m cpu = %d\n", MINIMUM_CPU_POWERS[i]);
		seq_printf(m, " M cpu = %d\n", MAXIMUM_CPU_POWERS[i]);
		seq_printf(m, " m gpu = %d\n", MINIMUM_GPU_POWERS[i]);
		seq_printf(m, " M gpu = %d\n", MAXIMUM_GPU_POWERS[i]);
	}


	return 0;
}

static ssize_t tscpu_write_dtm_setting(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	char desc[128];
	/* char arg_name[32] = {0}; */
	/* int arg_val = 0; */
	int len = 0;

	int i_id = -1, i_first_step = -1, i_theta_r = -1, i_theta_f = -1, i_budget_change =
		-1, i_min_cpu_pwr = -1, i_max_cpu_pwr = -1, i_min_gpu_pwr = -1, i_max_gpu_pwr = -1;


	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;

	desc[len] = '\0';

	if (9 <= sscanf(desc, "%d %d %d %d %d %d %d %d %d", &i_id, &i_first_step, &i_theta_r, &i_theta_f,
				&i_budget_change, &i_min_cpu_pwr, &i_max_cpu_pwr, &i_min_gpu_pwr,
				&i_max_gpu_pwr)) {
		tscpu_printk("tscpu_write_dtm_setting input %d %d %d %d %d %d %d %d %d\n", i_id,
				i_first_step, i_theta_r, i_theta_f, i_budget_change, i_min_cpu_pwr,
				i_max_cpu_pwr, i_min_gpu_pwr, i_max_gpu_pwr);

		if (i_id >= 0 && i_id < MAX_CPT_ADAPTIVE_COOLERS) {
			if (i_first_step > 0)
				FIRST_STEP_TOTAL_POWER_BUDGETS[i_id] = i_first_step;
			if (i_theta_r > 0)
				PACKAGE_THETA_JA_RISES[i_id] = i_theta_r;
			if (i_theta_f > 0)
				PACKAGE_THETA_JA_FALLS[i_id] = i_theta_f;
			if (i_budget_change > 0)
				MINIMUM_BUDGET_CHANGES[i_id] = i_budget_change;
			if (i_min_cpu_pwr > 0)
				MINIMUM_CPU_POWERS[i_id] = i_min_cpu_pwr;
			if (i_max_cpu_pwr > 0)
				MAXIMUM_CPU_POWERS[i_id] = i_max_cpu_pwr;
			if (i_min_gpu_pwr > 0)
				MINIMUM_GPU_POWERS[i_id] = i_min_gpu_pwr;
			if (i_max_gpu_pwr > 0)
				MAXIMUM_GPU_POWERS[i_id] = i_max_gpu_pwr;

			active_adp_cooler = i_id;

			tscpu_printk("tscpu_write_dtm_setting applied %d %d %d %d %d %d %d %d %d\n",
					i_id, FIRST_STEP_TOTAL_POWER_BUDGETS[i_id],
					PACKAGE_THETA_JA_RISES[i_id], PACKAGE_THETA_JA_FALLS[i_id],
					MINIMUM_BUDGET_CHANGES[i_id], MINIMUM_CPU_POWERS[i_id],
					MAXIMUM_CPU_POWERS[i_id], MINIMUM_GPU_POWERS[i_id],
					MAXIMUM_GPU_POWERS[i_id]);
		} else {
			tscpu_dprintk("tscpu_write_dtm_setting out of range\n");
		}

		/* MINIMUM_TOTAL_POWER = MINIMUM_CPU_POWER + MINIMUM_GPU_POWER; */
		/* MAXIMUM_TOTAL_POWER = MAXIMUM_CPU_POWER + MAXIMUM_GPU_POWER; */

		return count;
	}
	tscpu_dprintk("tscpu_write_dtm_setting bad argument\n");

	return -EINVAL;
}

static int tscpu_read_gpu_threshold(struct seq_file *m, void *v)
{
	/* int i; */


	seq_printf(m, "H %d L %d\n", GPU_L_H_TRIP, GPU_L_L_TRIP);


	return 0;
}


static ssize_t tscpu_write_gpu_threshold(struct file *file, const char __user *buffer,
		size_t count, loff_t *data)
{
	char desc[128];
	int len = 0;

	int gpu_h = -1, gpu_l = -1;


	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;

	desc[len] = '\0';

	if (2 <= sscanf(desc, "%d %d", &gpu_h, &gpu_l)) {
		tscpu_printk("tscpu_write_gpu_threshold input %d %d\n", gpu_h, gpu_l);

		if ((gpu_h > 0) && (gpu_l > 0) && (gpu_h > gpu_l)) {
			GPU_L_H_TRIP = gpu_h;
			GPU_L_L_TRIP = gpu_l;

			tscpu_printk("tscpu_write_gpu_threshold applied %d %d\n", GPU_L_H_TRIP,
					GPU_L_L_TRIP);
		} else {
			tscpu_dprintk("tscpu_write_gpu_threshold out of range\n");
		}

		return count;
	}
	tscpu_dprintk("tscpu_write_gpu_threshold bad argument\n");

	return -EINVAL;
}

#if THERMAL_HEADROOM
static int tscpu_read_thp(struct seq_file *m, void *v)
{
	seq_printf(m, "Tpcb pt coef %d\n", p_Tpcb_correlation);
	seq_printf(m, "Tpcb threshold %d\n", Tpcb_trip_point);
	seq_printf(m, "Tj pt coef %d\n", thp_p_tj_correlation);
	seq_printf(m, "thp tj threshold %d\n", thp_threshold_tj);

	return 0;
}


static ssize_t tscpu_write_thp(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	char desc[128];
	int len = 0;
	int tpcb_coef = -1, tpcb_trip = -1, thp_coef = -1, thp_threshold = -1;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;
	desc[len] = '\0';

	if (4 <= sscanf(desc, "%d %d %d %d", &tpcb_coef, &tpcb_trip, &thp_coef, &thp_threshold)) {
		tscpu_printk("%s input %d %d %d %d\n", __func__, tpcb_coef, tpcb_trip, thp_coef,
				thp_threshold);

		p_Tpcb_correlation = tpcb_coef;
		Tpcb_trip_point = tpcb_trip;
		thp_p_tj_correlation = thp_coef;
		thp_threshold_tj = thp_threshold;

		return count;
	}
	tscpu_dprintk("%s bad argument\n", __func__);

	return -EINVAL;
}
#endif

#endif

#if MTKTSCPU_FAST_POLLING
static int tscpu_read_fastpoll(struct seq_file *m, void *v)
{
	/* int i; */


	seq_printf(m, "trip %d factor %d\n", fast_polling_trip_temp, fast_polling_factor);



	return 0;
}


static ssize_t tscpu_write_fastpoll(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	char desc[128];
	int len = 0;

	int trip = -1, factor = -1;


	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;
	desc[len] = '\0';

	if (2 <= sscanf(desc, "%d %d", &trip, &factor)) {
		tscpu_printk("tscpu_write_fastpoll input %d %d\n", trip, factor);

		if ((trip >= 0) && (factor > 0)) {
			fast_polling_trip_temp = trip;
			fast_polling_factor = factor;

			tscpu_printk("tscpu_write_fastpoll applied %d %d\n", fast_polling_trip_temp,
					fast_polling_factor);
		} else {
			tscpu_dprintk("tscpu_write_fastpoll out of range\n");
		}

		return count;
	}
	tscpu_dprintk("tscpu_write_fastpoll bad argument\n");

	return -EINVAL;
}
#endif

static ssize_t tscpu_write(struct file *file, const char __user *buffer, size_t count,
		loff_t *data)
{
	int len = 0, time_msec = 0;
	int trip[10] = { 0 };
	int t_type[10] = { 0 };
	int i;
	char bind0[20], bind1[20], bind2[20], bind3[20], bind4[20];
	char bind5[20], bind6[20], bind7[20], bind8[20], bind9[20];
	char desc[512];

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return 0;
	desc[len] = '\0';

	if (sscanf
			(desc,
			 "%d %d %d %s %d %d %s %d %d %s %d %d %s %d %d %s %d %d %s %d %d %s %d %d %s %d %d %s %d %d %s %d %d",
			 &num_trip, &trip[0], &t_type[0], bind0, &trip[1], &t_type[1], bind1, &trip[2],
			 &t_type[2], bind2, &trip[3], &t_type[3], bind3, &trip[4], &t_type[4], bind4, &trip[5],
			 &t_type[5], bind5, &trip[6], &t_type[6], bind6, &trip[7], &t_type[7], bind7, &trip[8],
			 &t_type[8], bind8, &trip[9], &t_type[9], bind9, &time_msec, &MA_len_temp) == 33) {

		tscpu_dprintk("tscpu_write tscpu_unregister_thermal MA_len_temp=%d\n", MA_len_temp);

		/*      modify for PTPOD, if disable Thermal,
			PTPOD still need to use this function for getting temperature
		 */

		tscpu_unregister_thermal();


		for (i = 0; i < num_trip; i++)
			g_THERMAL_TRIP[i] = t_type[i];

		g_bind0[0] = g_bind1[0] = g_bind2[0] = g_bind3[0] = g_bind4[0] = g_bind5[0] =
			g_bind6[0] = g_bind7[0] = g_bind8[0] = g_bind9[0] = '\0';

		for (i = 0; i < 20; i++) {
			g_bind0[i] = bind0[i];
			g_bind1[i] = bind1[i];
			g_bind2[i] = bind2[i];
			g_bind3[i] = bind3[i];
			g_bind4[i] = bind4[i];
			g_bind5[i] = bind5[i];
			g_bind6[i] = bind6[i];
			g_bind7[i] = bind7[i];
			g_bind8[i] = bind8[i];
			g_bind9[i] = bind9[i];
		}

#if CPT_ADAPTIVE_AP_COOLER
		/* initialize... */
		for (i = 0; i < MAX_CPT_ADAPTIVE_COOLERS; i++)
			TARGET_TJS[i] = 117000;

		if (!strncmp(bind0, adaptive_cooler_name, 13))
			TARGET_TJS[(bind0[13] - '0')] = trip[0];

		if (!strncmp(bind1, adaptive_cooler_name, 13))
			TARGET_TJS[(bind1[13] - '0')] = trip[1];

		if (!strncmp(bind2, adaptive_cooler_name, 13))
			TARGET_TJS[(bind2[13] - '0')] = trip[2];

		if (!strncmp(bind3, adaptive_cooler_name, 13))
			TARGET_TJS[(bind3[13] - '0')] = trip[3];

		if (!strncmp(bind4, adaptive_cooler_name, 13))
			TARGET_TJS[(bind4[13] - '0')] = trip[4];

		if (!strncmp(bind5, adaptive_cooler_name, 13))
			TARGET_TJS[(bind5[13] - '0')] = trip[5];

		if (!strncmp(bind6, adaptive_cooler_name, 13))
			TARGET_TJS[(bind6[13] - '0')] = trip[6];

		if (!strncmp(bind7, adaptive_cooler_name, 13))
			TARGET_TJS[(bind7[13] - '0')] = trip[7];

		if (!strncmp(bind8, adaptive_cooler_name, 13))
			TARGET_TJS[(bind8[13] - '0')] = trip[8];

		if (!strncmp(bind9, adaptive_cooler_name, 13))
			TARGET_TJS[(bind9[13] - '0')] = trip[9];

		tscpu_dprintk("tscpu_write TTJ0=%d, TTJ1=%d, TTJ2=%d\n", TARGET_TJS[0],
				TARGET_TJS[1], TARGET_TJS[2]);
#endif

		tscpu_dprintk("tscpu_write g_THERMAL_TRIP_0=%d,g_THERMAL_TRIP_1=%d,g_THERMAL_TRIP_2=%d,",
				g_THERMAL_TRIP[0], g_THERMAL_TRIP[1], g_THERMAL_TRIP[2]);
		tscpu_dprintk("g_THERMAL_TRIP_3=%d,g_THERMAL_TRIP_4=%d,g_THERMAL_TRIP_5=%d,",
				g_THERMAL_TRIP[3], g_THERMAL_TRIP[4], g_THERMAL_TRIP[5]);
		tscpu_dprintk("g_THERMAL_TRIP_6=%d,g_THERMAL_TRIP_7=%d,g_THERMAL_TRIP_8=%d,g_THERMAL_TRIP_9=%d,\n",
				g_THERMAL_TRIP[6], g_THERMAL_TRIP[7], g_THERMAL_TRIP[8],	g_THERMAL_TRIP[9]);
		tscpu_dprintk("tscpu_write cooldev0=%s,cooldev1=%s,cooldev2=%s,cooldev3=%s,cooldev4=%s,",
				g_bind0, g_bind1, g_bind2, g_bind3, g_bind4);
		tscpu_dprintk("cooldev5=%s,cooldev6=%s,cooldev7=%s,cooldev8=%s,cooldev9=%s\n",
				g_bind5, g_bind6, g_bind7, g_bind8,	g_bind9);

		for (i = 0; i < num_trip; i++)
			trip_temp[i] = trip[i];

		interval = time_msec;

		tscpu_dprintk("tscpu_write trip_0_temp=%d,trip_1_temp=%d,trip_2_temp=%d,",
									trip_temp[0], trip_temp[1], trip_temp[2]);
		tscpu_dprintk("trip_3_temp=%d,trip_4_temp=%d,trip_5_temp=%d,",
									trip_temp[3], trip_temp[4], trip_temp[5]);
		tscpu_dprintk("trip_6_temp=%d,trip_7_temp=%d,trip_8_temp=%d,",
									trip_temp[6],	trip_temp[7], trip_temp[8]);
		tscpu_dprintk("trip_9_temp=%d,time_ms=%d, num_trip=%d\n",
									trip_temp[9], interval, num_trip);


		/* get temp, set high low threshold */
		/*
		   curr_temp = get_immediate_temp();
		   for(i=0; i<num_trip; i++)
		   {
		   if(curr_temp>trip_temp[i])
		   break;
		   }
		   if(i==0)
		   {
		   tscpu_printk("tscpu_write setting error");
		   }
		   else if(i==num_trip)
		   set_high_low_threshold(trip_temp[i-1], 10000);
		   else
		   set_high_low_threshold(trip_temp[i-1], trip_temp[i]);
		 */
		tscpu_dprintk("tscpu_write tscpu_register_thermal\n");
		tscpu_register_thermal();

		proc_write_flag = 1;

		return count;
	}
	tscpu_dprintk("tscpu_write bad argument\n");

	return -EINVAL;
}



int tscpu_register_DVFS_hotplug_cooler(void)
{

	int i;

	tscpu_dprintk("tscpu_register_DVFS_hotplug_cooler\n");
	for (i = 0; i < Num_of_OPP; i++) {
		cl_dev[i] = mtk_thermal_cooling_device_register(&cooler_name[i * 20], NULL,
				&mtktscpu_cooling_F0x2_ops);
	}
	cl_dev_sysrst = mtk_thermal_cooling_device_register("mtktscpu-sysrst", NULL,
			&mtktscpu_cooling_sysrst_ops);
#if CPT_ADAPTIVE_AP_COOLER
	cl_dev_adp_cpu[0] = mtk_thermal_cooling_device_register("cpu_adaptive_0", NULL,
			&mtktscpu_cooler_adp_cpu_ops);

	cl_dev_adp_cpu[1] = mtk_thermal_cooling_device_register("cpu_adaptive_1", NULL,
			&mtktscpu_cooler_adp_cpu_ops);

	cl_dev_adp_cpu[2] = mtk_thermal_cooling_device_register("cpu_adaptive_2", NULL,
			&mtktscpu_cooler_adp_cpu_ops);
#endif


	return 0;
}

static int tscpu_register_thermal(void)
{

	tscpu_dprintk("tscpu_register_thermal\n");

	/* trips : trip 0~3 */
	thz_dev = mtk_thermal_zone_device_register("mtktscpu", num_trip, NULL,
			&mtktscpu_dev_ops, 0, 0, 0, interval);

	return 0;
}

void tscpu_unregister_DVFS_hotplug_cooler(void)
{

	int i;

	for (i = 0; i < Num_of_OPP; i++) {
		if (cl_dev[i]) {
			mtk_thermal_cooling_device_unregister(cl_dev[i]);
			cl_dev[i] = NULL;
		}
	}
	if (cl_dev_sysrst) {
		mtk_thermal_cooling_device_unregister(cl_dev_sysrst);
		cl_dev_sysrst = NULL;
	}
#if CPT_ADAPTIVE_AP_COOLER
	if (cl_dev_adp_cpu[0]) {
		mtk_thermal_cooling_device_unregister(cl_dev_adp_cpu[0]);
		cl_dev_adp_cpu[0] = NULL;
	}

	if (cl_dev_adp_cpu[1]) {
		mtk_thermal_cooling_device_unregister(cl_dev_adp_cpu[1]);
		cl_dev_adp_cpu[1] = NULL;
	}

	if (cl_dev_adp_cpu[2]) {
		mtk_thermal_cooling_device_unregister(cl_dev_adp_cpu[2]);
		cl_dev_adp_cpu[2] = NULL;
	}
#endif


}

static void tscpu_unregister_thermal(void)
{

	tscpu_dprintk("tscpu_unregister_thermal\n");
	if (thz_dev) {
		mtk_thermal_zone_device_unregister(thz_dev);
		thz_dev = NULL;
	}

}

/* pause ALL periodoc temperature sensing point */
static void thermal_pause_all_periodoc_temp_sensing(void)
{
	int i = 0;
	unsigned long flags;
	int temp;

	/* tscpu_printk("thermal_pause_all_periodoc_temp_sensing\n"); */

	mt_ptp_lock(&flags);

	/*config bank0,1,2 */
	for (i = 0; i < THERMAL_BANK_NUM; i++) {

		mtktscpu_switch_bank(i);
		temp = DRV_Reg32(TEMPMSRCTL1);
		/* set bit8=bit1=bit2=bit3=1 to pause sensing point 0,1,2,3 */
		DRV_WriteReg32(TEMPMSRCTL1, (temp | 0x10E));
	}

	mt_ptp_unlock(&flags);

}


/* release ALL periodoc temperature sensing point */
static void thermal_release_all_periodoc_temp_sensing(void)
{
	int i = 0;
	unsigned long flags;
	int temp;

	/* tscpu_printk("thermal_release_all_periodoc_temp_sensing\n"); */

	mt_ptp_lock(&flags);

	/*config bank0,1,2 */
	for (i = 0; i < THERMAL_BANK_NUM; i++) {

		mtktscpu_switch_bank(i);
		temp = DRV_Reg32(TEMPMSRCTL1);
		/* set bit1=bit2=bit3=0 to release sensing point 0,1,2 */
		DRV_WriteReg32(TEMPMSRCTL1, ((temp & (~0x10E))));
	}

	mt_ptp_unlock(&flags);

}


/* disable ALL periodoc temperature sensing point */
static void thermal_disable_all_periodoc_temp_sensing(void)
{
	int i = 0;
	unsigned long flags;

	/* tscpu_printk("thermal_disable_all_periodoc_temp_sensing\n"); */

	mt_ptp_lock(&flags);

	/*config bank0,1,2 */
	for (i = 0; i < THERMAL_BANK_NUM; i++) {

		mtktscpu_switch_bank(i);
		/* tscpu_printk("thermal_disable_all_periodoc_temp_sensing:Bank_%d\n",i); */
		THERMAL_WRAP_WR32(0x00000000, TEMPMONCTL0);
	}

	mt_ptp_unlock(&flags);

}

static void tscpu_clear_all_temp(void)
{
	CPU_TS_MCU1_T = 0;
	CPU_TS_MCU2_T = 0;
	GPU_TS_MCU1_T = 0;
	GPU_TS_MCU2_T = 0;
	SOC_TS_MCU1_T = 0;
	SOC_TS_MCU2_T = 0;
	SOC_TS_MCU3_T = 0;



}

/*tscpu_thermal_suspend spend 1000us~1310us*/
static int tscpu_thermal_suspend(struct platform_device *dev, pm_message_t state)
{
	int cnt = 0;
	int temp = 0, wd_api_ret;
	struct wd_api *wd_api;

	tscpu_printk("tscpu_thermal_suspend, talking_flag=%d\n", talking_flag);
#if THERMAL_PERFORMANCE_PROFILE
	struct timeval begin, end;
	unsigned long val;

	do_gettimeofday(&begin);
#endif

	g_tc_resume = 1;	/* set "1", don't read temp during suspend */

	if (talking_flag == false) {
		tscpu_dprintk("tscpu_thermal_suspend no talking\n");

		wd_api_ret = get_wd_api(&wd_api);
		if (wd_api_ret >= 0) {
			/* reset mode */
			wd_api->wd_thermal_direct_mode_config(WD_REQ_DIS, WD_REQ_IRQ_MODE);
		}

		while (cnt < 50) {
			temp = (DRV_Reg32(THAHBST0) >> 16);
			if (cnt > 10)
				tscpu_printk(KERN_CRIT "THAHBST0 = 0x%x,cnt=%d, %d\n", temp, cnt,
						__LINE__);
			if (temp == 0x0) {
				/* pause all periodoc temperature sensing point 0~2 */
				thermal_pause_all_periodoc_temp_sensing();	/* TEMPMSRCTL1 */
				break;
			}
			udelay(2);
			cnt++;
		}

		/* disable periodic temp measurement on sensor 0~2 */
		thermal_disable_all_periodoc_temp_sensing();	/* TEMPMONCTL0 */

		tscpu_thermal_clock_off();

		/*TSCON1[5:4]=2'b11, Buffer off */
		/* turn off the sensor buffer to save power */
		THERMAL_WRAP_WR32(DRV_Reg32(TS_CON1) | 0x00000030, TS_CON1);
	}
#if THERMAL_PERFORMANCE_PROFILE
	do_gettimeofday(&end);

	/* Get milliseconds */
	tscpu_printk("suspend time spent, sec : %lu , usec : %lu\n", (end.tv_sec - begin.tv_sec),
			(end.tv_usec - begin.tv_usec));
#endif
	return 0;
}

/*tscpu_thermal_suspend spend 3000us~4000us*/
static int tscpu_thermal_resume(struct platform_device *dev)
{
	int temp = 0;
	int cnt = 0;

	tscpu_printk("tscpu_thermal_resume,talking_flag=%d\n", talking_flag);

	g_tc_resume = 1;	/* set "1", don't read temp during start resume */

	if (talking_flag == false) {

		tscpu_reset_thermal();
		tscpu_thermal_clock_on();

		temp = DRV_Reg32(TS_CON1);
		temp &= ~(0x00000030);	/* TS_CON1[5:4]=2'b00,   00: Buffer on, TSMCU to AUXADC */
		THERMAL_WRAP_WR32(temp, TS_CON1);	/* read abb need */
		/* RG_TS2AUXADC < set from 2'b11 to 2'b00 when resume.
				wait 100uS than turn on thermal controller. */
		udelay(200);

		/*add this function to read all temp first to avoid
		  write TEMPPROTTC first time will issue an fake signal to RGU */
		tscpu_fast_initial_sw_workaround();

		while (cnt < 50) {
			temp = (DRV_Reg32(THAHBST0) >> 16);
			if (cnt > 10)
				tscpu_printk(KERN_CRIT "THAHBST0 = 0x%x,cnt=%d, %d\n", temp, cnt,
						__LINE__);
			if (temp == 0x0) {
				/* pause all periodoc temperature sensing point 0~2 */
				thermal_pause_all_periodoc_temp_sensing();	/* TEMPMSRCTL1 */
				break;
			}
			udelay(2);
			cnt++;
		}
		thermal_disable_all_periodoc_temp_sensing();	/* TEMPMONCTL0 */

		thermal_initial_all_bank();

		thermal_release_all_periodoc_temp_sensing();	/* must release before start */



		tscpu_clear_all_temp();

		read_all_bank_temperature();
		/*tscpu_config_all_tc_hw_protect(trip_temp[0], tc_mid_trip);*/

	}

	g_tc_resume = 2;	/* set "2", resume finish,can read temp */

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id mt_thermal_of_match[] = {
	{.compatible = "mediatek,mt8163-thermal",},
	{},
};
#endif


#if 0
/*must wait until AUXADC initial ready*/
static int thermal_prob(struct platform_device *dev)
{

	tscpu_printk(KERN_CRIT "thermal_prob\n");

	/*
	   default is dule mode(irq/reset), if not to config this and hot happen,
	   system will reset after 30 secs

	   Thermal need to config to direct reset mode
	   this API provide by Weiqi Fu(RGU SW owner).
	 */
	tscpu_config_all_tc_hw_protect(trip_temp[0], tc_mid_trip);


	return 0;
}
#endif


#if MTK_TS_CPU_RT
static int ktp_limited = -275000;

static int ktp_thread(void *arg)
{
	int max_temp = 0;

	int bank0_T;
	int bank1_T;
	int bank2_T;
	/* int bank3_T; */
	int curr_temp1;
	int curr_temp2;


	struct sched_param param = {.sched_priority = 98 };

	sched_setscheduler(current, SCHED_FIFO, &param);
	set_current_state(TASK_INTERRUPTIBLE);

	tscpu_printk("ktp_thread 1st run\n");


	schedule();

	for (;;) {
		int temp_tc_mid_trip = tc_mid_trip;
		int temp_ktp_limited = ktp_limited;

		tscpu_printk("ktp_thread awake,tc_mid_trip=%d\n", tc_mid_trip);
		if (kthread_should_stop())
			break;


		bank0_T = MAX(CPU_TS_MCU1_T, CPU_TS_MCU2_T);
		bank1_T = GPU_TS_MCU2_T;
		bank2_T = MAX(SOC_TS_MCU1_T, SOC_TS_MCU2_T);
		bank2_T = MAX(bank2_T, SOC_TS_MCU3_T);

		curr_temp1 = MAX(bank0_T, bank1_T);
		curr_temp2 = MAX(bank2_T, curr_temp1);
		max_temp = curr_temp2;

		tscpu_printk("ktp_thread temp=%d\n", max_temp);
		/* trip_temp[1] should be shutdown point... */
		if ((temp_tc_mid_trip > -275000) && (max_temp >= (temp_tc_mid_trip - 5000))) {
			/* Do what ever we want */
			tscpu_printk("ktp_thread overheat %d\n", max_temp);

			/* freq/volt down or cpu down or backlight down or charging down... */
			thermal_budget_notify(700);
			mt_gpufreq_thermal_protect(300);

			ktp_limited = temp_tc_mid_trip;

			msleep(20 * 1000);
		} else if ((temp_ktp_limited > -275000) && (max_temp < temp_ktp_limited)) {
			unsigned int final_limit;

			final_limit = MIN(static_cpu_power_limit, adaptive_cpu_power_limit);
			tscpu_printk("ktp_thread unlimit cpu=%d\n", final_limit);
			thermal_budget_notify((final_limit != 0x7FFFFFFF) ? final_limit : 0);


			final_limit = MIN(static_gpu_power_limit, adaptive_gpu_power_limit);
			tscpu_printk("ktp_thread unlimit gpu=%d\n", final_limit);
			mt_gpufreq_thermal_protect((final_limit != 0x7FFFFFFF) ? final_limit : 0);

			ktp_limited = -275000;

			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		} else {
			tscpu_printk("ktp_thread else temp=%d, trip=%d, ltd=%d\n", max_temp,
					temp_tc_mid_trip, temp_ktp_limited);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		}
	}

	tscpu_printk("ktp_thread stopped\n");

	return 0;
}
#endif


#if THERMAL_GPIO_OUT_TOGGLE
static int tscpu_GPIO_out(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_GPIO_out, NULL);
}

static const struct file_operations mtktscpu_GPIO_out_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_GPIO_out,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write_GPIO_out,
	.release = single_release,
};
#endif



static int tscpu_Tj_out(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_Tj_out, NULL);
}

static const struct file_operations mtktscpu_Tj_out_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_Tj_out,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write_Tj_out,
	.release = single_release,
};



static int tscpu_open_opp(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_opp, NULL);
}

static const struct file_operations mtktscpu_opp_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_open_opp,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tscpu_open_log(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_log, NULL);
}

static const struct file_operations mtktscpu_log_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_open_log,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write_log,
	.release = single_release,
};

static int tscpu_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read, NULL);
}

static const struct file_operations mtktscpu_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write,
	.release = single_release,
};



static int tscpu_cal_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_cal, NULL);
}

static const struct file_operations mtktscpu_cal_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_cal_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


static int tscpu_read_temperature_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_temperature_info, NULL);
}

static const struct file_operations mtktscpu_read_temperature_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_read_temperature_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write,
	.release = single_release,
};


static int tscpu_set_temperature_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_set_temperature_read, NULL);
}

static const struct file_operations mtktscpu_set_temperature_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_set_temperature_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_set_temperature_write,
	.release = single_release,
};


static int tscpu_talking_flag_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_talking_flag_read, NULL);
}

static const struct file_operations mtktscpu_talking_flag_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_talking_flag_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_talking_flag_write,
	.release = single_release,
};

#if CPT_ADAPTIVE_AP_COOLER

/* +ASC+ */
static int tscpu_open_atm(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_atm, NULL);
}

static const struct file_operations mtktscpu_atm_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_open_atm,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write_atm,
	.release = single_release,
};

/* -ASC- */

static int tscpu_dtm_setting_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_dtm_setting, NULL);
}

static const struct file_operations mtktscpu_dtm_setting_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_dtm_setting_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write_dtm_setting,
	.release = single_release,
};


static int tscpu_gpu_threshold_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_gpu_threshold, NULL);
}

static const struct file_operations mtktscpu_gpu_threshold_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_gpu_threshold_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write_gpu_threshold,
	.release = single_release,
};

#if THERMAL_HEADROOM
static int tscpu_thp_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_thp, NULL);
}

static const struct file_operations mtktscpu_thp_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_thp_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write_thp,
	.release = single_release,
};
#endif

#endif



#if MTKTSCPU_FAST_POLLING
static int tscpu_fastpoll_open(struct inode *inode, struct file *file)
{
	return single_open(file, tscpu_read_fastpoll, NULL);
}

static const struct file_operations mtktscpu_fastpoll_fops = {
	.owner = THIS_MODULE,
	.open = tscpu_fastpoll_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tscpu_write_fastpoll,
	.release = single_release,
};
#endif


static int mtktscpu_switch_bank(enum thermal_bank_name bank)
{
	/* tscpu_dprintk( "mtktscpu_switch_bank =bank=%d\n",bank); */

	switch (bank) {
	case THERMAL_BANK0:	/* CPU (TS_MCU1,TS_MCU2)          (TS3, TS4) */
			thermal_clrl(PTPCORESEL, 0xF);	/* bank0 */
			break;
	case THERMAL_BANK1:	/* GPU (TS_MCU3)                                  (TS5) */
			thermal_clrl(PTPCORESEL, 0xF);
			thermal_setl(PTPCORESEL, 0x1);	/* bank1 */
			break;
	case THERMAL_BANK2:	/* SOC (TS_MCU4,TS_MCU2,TS_MCU3)  (TS1, TS4, TS5) */
			thermal_clrl(PTPCORESEL, 0xF);
			thermal_setl(PTPCORESEL, 0x2);	/* bank2 */
			break;
	default:
			thermal_clrl(PTPCORESEL, 0xF);	/* bank0 */
			break;
	}
	return 0;
}



static void thermal_initial_all_bank(void)
{
	int i = 0;
	unsigned long flags;
	UINT32 temp = 0;
	/* tscpu_printk("thermal_initial_all_bank,THERMAL_BANK_NUM=%d\n",THERMAL_BANK_NUM); */

	/* tscpu_thermal_clock_on(); */

	mt_ptp_lock(&flags);

	/* AuxADC Initialization,ref MT6592_AUXADC.doc  TODO: check this line */
	temp = DRV_Reg32(AUXADC_CON0_V);	/* Auto set enable for CH11 */
	temp &= 0xFFFFF7FF;	/* 0: Not AUTOSET mode */
	THERMAL_WRAP_WR32(temp, AUXADC_CON0_V);	/* disable auxadc channel 11 synchronous mode */
	THERMAL_WRAP_WR32(0x800, AUXADC_CON1_CLR_V);	/* disable auxadc channel 11 immediate mode */


	for (i = 0; i < THERMAL_BANK_NUM; i++) {
		mtktscpu_switch_bank(i);
		thermal_reset_and_initial();
		thermal_config_TS_in_banks(i);
	}

	/* enable auxadc channel 11 immediate mode */
	THERMAL_WRAP_WR32(0x800, AUXADC_CON1_SET_V);

	for (i = 0; i < THERMAL_BANK_NUM; i++) {
		mtktscpu_switch_bank(i);
		thermal_enable_all_periodoc_sensing_point(i);
	}

	mt_ptp_unlock(&flags);

}


static void tscpu_config_all_tc_hw_protect(int temperature, int temperature2)
{
	int i = 0, wd_api_ret;
	unsigned long flags;
	struct wd_api *wd_api;

	tscpu_dprintk("tscpu_config_all_tc_hw_protect,temperature=%d,temperature2=%d,\n",
			temperature, temperature2);

#if THERMAL_PERFORMANCE_PROFILE
	struct timeval begin, end;
	unsigned long val;

	do_gettimeofday(&begin);
#endif
	/*spend 860~1463 us */
	/*Thermal need to config to direct reset mode
	  this API provide by Weiqi Fu(RGU SW owner). */

	wd_api_ret = get_wd_api(&wd_api);
	if (wd_api_ret >= 0) {
		/* reset mode */
		wd_api->wd_thermal_direct_mode_config(WD_REQ_DIS, WD_REQ_RST_MODE);
	} else {
		pr_err("%d FAILED TO GET WD API\n", __LINE__);
		BUG();
	}

#if THERMAL_PERFORMANCE_PROFILE
	do_gettimeofday(&end);

	/* Get milliseconds */
	tscpu_printk("resume time spent, sec : %lu , usec : %lu\n", (end.tv_sec - begin.tv_sec),
			(end.tv_usec - begin.tv_usec));
#endif

	mt_ptp_lock(&flags);


	for (i = 0; i < THERMAL_BANK_NUM; i++) {

		mtktscpu_switch_bank(i);

		set_tc_trigger_hw_protect(temperature, temperature2);	/* Move thermal HW protection ahead... */
	}

	mt_ptp_unlock(&flags);

	/*Thermal need to config to direct reset mode
	  this API provide by Weiqi Fu(RGU SW owner). */
	/* reset mode */
	wd_api->wd_thermal_direct_mode_config(WD_REQ_EN, WD_REQ_RST_MODE);

}

static void tscpu_reset_thermal(void)
{
	int temp = 0;
	/* reset thremal ctrl */
	temp = DRV_Reg32(INFRA_GLOBALCON_RST_0_SET);
	temp |= 0x00000001;	/* 1: Enables thermal control software reset */
	THERMAL_WRAP_WR32(temp, INFRA_GLOBALCON_RST_0_SET);

	/* un reset */
	temp = DRV_Reg32(INFRA_GLOBALCON_RST_0_CLR);
	temp |= 0x00000001;	/* 1: Enable reset Disables thermal control software reset */
	THERMAL_WRAP_WR32(temp, INFRA_GLOBALCON_RST_0_CLR);
}

static void tscpu_fast_initial_sw_workaround(void)
{
	int i = 0;
	unsigned long flags;
	/* tscpu_printk("tscpu_fast_initial_sw_workaround\n"); */

	/* tscpu_thermal_clock_on(); */

	mt_ptp_lock(&flags);

	for (i = 0; i < THERMAL_BANK_NUM; i++) {
		mtktscpu_switch_bank(i);
		thermal_fast_init();
	}

	mt_ptp_unlock(&flags);

}


#if THERMAL_DRV_UPDATE_TEMP_DIRECT_TO_MET
int tscpu_get_cpu_temp_met(enum MTK_THERMAL_SENSOR_CPU_ID_MET id)
{
	unsigned long flags;
	int ret;

	if (id < 0 || id >= MTK_THERMAL_SENSOR_CPU_COUNT)
		return -127000;
	else if (ATM_CPU_LIMIT == id)
		return (adaptive_cpu_power_limit != 0x7FFFFFFF) ? adaptive_cpu_power_limit : 0;
	else if (ATM_GPU_LIMIT == id)
		return (adaptive_gpu_power_limit != 0x7FFFFFFF) ? adaptive_gpu_power_limit : 0;

	tscpu_met_lock(&flags);
	if (a_tscpu_all_temp[id] == 0) {
		tscpu_met_unlock(&flags);
		return -127000;
	}
	ret = a_tscpu_all_temp[id];

	tscpu_met_unlock(&flags);
	return ret;
}
EXPORT_SYMBOL(tscpu_get_cpu_temp_met);
#endif



static enum hrtimer_restart tscpu_update_tempinfo(struct hrtimer *timer)
{
	ktime_t ktime;
	unsigned long flags;
	int resume_ok = 0;
	/* tscpu_printk("tscpu_update_tempinfo\n"); */
	tscpu_dprintk("%s: g_tc_resume=%d\n", __func__, g_tc_resume);

	if (g_tc_resume == 0) {
		read_all_bank_temperature();
		if (resume_ok) {
			tscpu_config_all_tc_hw_protect(trip_temp[0], tc_mid_trip);
			resume_ok = 0;
		}
	} else if (g_tc_resume == 2) {	/* resume ready */

		/* tscpu_printk("tscpu_update_tempinfo g_tc_resume==2\n"); */
		g_tc_resume = 0;
		resume_ok = 1;
	}
#if THERMAL_DRV_UPDATE_TEMP_DIRECT_TO_MET

	tscpu_met_lock(&flags);
	a_tscpu_all_temp[0] = CPU_TS_MCU1_T;	/* temp of TS_MCU1 */
	a_tscpu_all_temp[1] = GPU_TS_MCU2_T;	/* temp of TS_MCU2 */
	a_tscpu_all_temp[2] = SOC_TS_MCU3_T;	/* temp of TS_MCU3 */
	tscpu_met_unlock(&flags);

	if (NULL != g_pThermalSampler)
		g_pThermalSampler();
#endif


	ktime = ktime_set(0, 50000000);	/* 50ms */
	hrtimer_forward_now(timer, ktime);



#if 0
	tscpu_printk("\n\n");
	tscpu_printk("\n tscpu_update_tempinfo, T=%d,%d,%d,%d,%d,%d\n", CPU_TS_MCU1_T,
			CPU_TS_MCU2_T, GPU_TS_MCU3_T, SOC_TS_MCU4_T, SOC_TS_MCU2_T, SOC_TS_MCU3_T);

	tscpu_printk("Bank 0 : CPU  (TS_MCU1 = %d,TS_MCU2 = %d)\n", CPU_TS_MCU1_T, CPU_TS_MCU2_T);
	tscpu_printk("Bank 1 : GPU  (TS_MCU3 = %d)\n", GPU_TS_MCU3_T);
	tscpu_printk("Bank 2 : SOC  (TS_MCU4 = %d,TS_MCU2 = %d,TS_MCU3 = %d)\n", SOC_TS_MCU4_T,
			SOC_TS_MCU2_T, SOC_TS_MCU3_T);

#endif

	return HRTIMER_RESTART;
}

static void tscpu_update_temperature_timer_init(void)
{
	ktime_t ktime;


	tscpu_dprintk("tscpu_update_temperature_timer_init\n");

	ktime = ktime_set(0, 50000000);	/* 50ms */

	hrtimer_init(&ts_tempinfo_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	ts_tempinfo_hrtimer.function = tscpu_update_tempinfo;
	hrtimer_start(&ts_tempinfo_hrtimer, ktime, HRTIMER_MODE_REL);

}

#if THERMAL_TURNOFF_AUXADC_BEFORE_DEEPIDLE
/*&#include <mach/mt_clkmgr.h>*/
/* #define MT_PDN_PERI_AUXADC MT_CG_INFRA_AUXADC */

void tscpu_pause_tc(void)
{
	int cnt = 0;
	int temp = 0;

	/* tscpu_printk("tscpu_pause_tc\n"); */


	g_tc_resume = 1;	/* set "1", don't read temp during suspend */

	/* if(talking_flag==false) */
	{
		/* tscpu_printk("tscpu_thermal_suspend no talking\n"); */

		while (cnt < 50) {
			temp = (DRV_Reg32(THAHBST0) >> 16);
			if (cnt > 10)
				tscpu_printk(KERN_CRIT "THAHBST0 = 0x%x,cnt=%d, %d\n", temp, cnt,
						__LINE__);
			if (temp == 0x0) {
				/* pause all periodoc temperature sensing point 0~2 */
				thermal_pause_all_periodoc_temp_sensing();	/* TEMPMSRCTL1 */
				break;
			}
			tscpu_printk(KERN_CRIT "1 THAHBST0 = 0x%x, %d\n", temp, __LINE__);
			udelay(2);
			cnt++;
		}

		/* disable periodic temp measurement on sensor 0~2 */
		thermal_disable_all_periodoc_temp_sensing();	/* TEMPMONCTL0 */


		/*TSCON1[5:4]=2'b11, Buffer off */
		/* THERMAL_WRAP_WR32(DRV_Reg32(TS_CON1) | 0x00000030, TS_CON1);
		turn off the sensor buffer to save power */
	}

}

void tscpu_release_tc(void)
{
	int temp = 0;
	int cnt = 0;

	/* tscpu_printk("tscpu_release_tc\n"); */

	g_tc_resume = 1;	/* set "1", don't read temp during start resume */

	/* if(talking_flag==false) */
	{

		tscpu_reset_thermal();

		/*add this function to read all temp first to avoid
		  write TEMPPROTTC first time will issue an fake signal to RGU */
		tscpu_fast_initial_sw_workaround();


		while (cnt < 50) {
			temp = (DRV_Reg32(THAHBST0) >> 16);
			if (cnt > 10)
				tscpu_printk(KERN_CRIT "THAHBST0 = 0x%x,cnt=%d, %d\n", temp, cnt,
						__LINE__);
			if (temp == 0x0) {
				/* pause all periodoc temperature sensing point 0~2 */
				thermal_pause_all_periodoc_temp_sensing();	/* TEMPMSRCTL1 */
				break;
			}
			udelay(2);
			cnt++;
		}
		thermal_disable_all_periodoc_temp_sensing();	/* TEMPMONCTL0 */

		thermal_initial_all_bank();

		thermal_release_all_periodoc_temp_sensing();	/* must release before start */


		tscpu_clear_all_temp();

		tscpu_config_all_tc_hw_protect(trip_temp[0], tc_mid_trip);


	}

	g_tc_resume = 2;	/* set "2", resume finish,can read temp */
}
#endif


void tscpu_cancel_thermal_timer(void)
{
	/* cancel timer */
	/* tscpu_dprintk("tscpu_update_temperature_timer_init\n"); */
	hrtimer_cancel(&ts_tempinfo_hrtimer);
#if THERMAL_TURNOFF_AUXADC_BEFORE_DEEPIDLE
	tscpu_pause_tc();

#if 1
	clk_disable_unprepare(clk_auxadc);
#else
	if (disable_clock(MT_CG_INFRA_AUXADC, "AUXADC"))
		tscpu_printk("hwEnableClock AUXADC failed\n");
#endif

#if defined(CONFIG_MTK_CLKMGR)
	if (clock_is_on(MT_CG_INFRA_AUXADC))
		tscpu_printk("hwEnableClock AUXADC still on\n");
#endif
#endif

	/* stop thermal framework polling when entering deep idle */
	if (thz_dev)
		cancel_delayed_work(&(thz_dev->poll_queue));
}


void tscpu_start_thermal_timer(void)
{
	ktime_t ktime;
#if THERMAL_TURNOFF_AUXADC_BEFORE_DEEPIDLE
#if 1
	if (clk_prepare_enable(clk_auxadc))
#else
	if (enable_clock(MT_CG_INFRA_AUXADC, "AUXADC"))
#endif
		tscpu_printk("hwEnableClock AUXADC failed\n");

	udelay(15);
#if defined(CONFIG_MTK_CLKMGR)
	if ((clock_is_on(MT_CG_INFRA_AUXADC) == 0x0))
		tscpu_printk("hwEnableClock AUXADC failed\n");
#endif

	/* if((DRV_Reg32(TS_CON1)& 0x00000030) == 0x11) */
	/* tscpu_printk("TS_CON1=0x%x\n",DRV_Reg32(TS_CON1)); */


	tscpu_release_tc();
#endif

	/* start timer */
	ktime = ktime_set(0, 50000000);	/* 50ms */
	hrtimer_start(&ts_tempinfo_hrtimer, ktime, HRTIMER_MODE_REL);

	/* resume thermal framework polling when leaving deep idle */
	if (thz_dev != NULL && interval != 0)
		/* 60ms */
		mod_delayed_work(system_freezable_wq, &(thz_dev->poll_queue), round_jiffies(msecs_to_jiffies(60)));
}

static int thermal_fast_init(void)
{
	UINT32 temp = 0;
	UINT32 cunt = 0;
	/* UINT32 temp1 = 0,temp2 = 0,temp3 = 0,count=0; */

	/* tscpu_printk( "thermal_fast_init\n"); */


	temp = 0xDA1;
	DRV_WriteReg32(PTPSPARE2, (0x00001000 + temp));	/* write temp to spare register */

	DRV_WriteReg32(TEMPMONCTL1, 1);	/* counting unit is 320 * 31.25us = 10ms */
	DRV_WriteReg32(TEMPMONCTL2, 1);	/* sensing interval is 200 * 10ms = 2000ms */
	DRV_WriteReg32(TEMPAHBPOLL, 1);	/* polling interval to check if temperature sense is ready */

	DRV_WriteReg32(TEMPAHBTO, 0x000000FF);	/* exceed this polling time, IRQ would be inserted */
	DRV_WriteReg32(TEMPMONIDET0, 0x00000000);	/* times for interrupt occurrance */
	DRV_WriteReg32(TEMPMONIDET1, 0x00000000);	/* times for interrupt occurrance */

	DRV_WriteReg32(TEMPMSRCTL0, 0x0000000);	/* temperature measurement sampling control */
	/* this value will be stored to TEMPPNPMUXADDR (TEMPSPARE0) automatically by hw */
	DRV_WriteReg32(TEMPADCPNP0, 0x1);
	DRV_WriteReg32(TEMPADCPNP1, 0x2);
	DRV_WriteReg32(TEMPADCPNP2, 0x3);
	DRV_WriteReg32(TEMPADCPNP3, 0x4);

#if 0
	DRV_WriteReg32(TEMPPNPMUXADDR, 0x1100B420);	/* AHB address for pnp sensor mux selection */
	DRV_WriteReg32(TEMPADCMUXADDR, 0x1100B420);	/* AHB address for auxadc mux selection */
	DRV_WriteReg32(TEMPADCENADDR, 0x1100B424);	/* AHB address for auxadc enable */
	DRV_WriteReg32(TEMPADCVALIDADDR, 0x1100B428);	/* AHB address for auxadc valid bit */
	DRV_WriteReg32(TEMPADCVOLTADDR, 0x1100B428);	/* AHB address for auxadc voltage output */

#else
	DRV_WriteReg32(TEMPPNPMUXADDR, (UINT32) PTPSPARE0_P);	/* AHB address for pnp sensor mux selection */
	DRV_WriteReg32(TEMPADCMUXADDR, (UINT32) PTPSPARE0_P);	/* AHB address for auxadc mux selection */
	DRV_WriteReg32(TEMPADCENADDR, (UINT32) PTPSPARE1_P);	/* AHB address for auxadc enable */
	DRV_WriteReg32(TEMPADCVALIDADDR, (UINT32) PTPSPARE2_P);	/* AHB address for auxadc valid bit */
	DRV_WriteReg32(TEMPADCVOLTADDR, (UINT32) PTPSPARE2_P);	/* AHB address for auxadc voltage output */
#endif
	DRV_WriteReg32(TEMPRDCTRL, 0x0);	/* read valid & voltage are at the same register */
	/* indicate where the valid bit is (the 12th bit is valid bit and 1 is valid) */
	DRV_WriteReg32(TEMPADCVALIDMASK, 0x0000002C);
	DRV_WriteReg32(TEMPADCVOLTAGESHIFT, 0x0);	/* do not need to shift */
	DRV_WriteReg32(TEMPADCWRITECTRL, 0x3);	/* enable auxadc mux & pnp write transaction */

	/* enable all interrupt except filter sense and immediate sense interrupt */
	DRV_WriteReg32(TEMPMONINT, 0x00000000);


	DRV_WriteReg32(TEMPMONCTL0, 0x0000000F);	/* enable all sensing point (sensing point 2 is unused) */


	cunt = 0;
	temp = DRV_Reg32(TEMPMSR0) & 0x0fff;
	while (temp != 0xDA1 && cunt < 20) {
		cunt++;
		tscpu_dprintk("[Power/CPU_Thermal]0 temp=%d,cunt=%d\n", temp, cunt);
		temp = DRV_Reg32(TEMPMSR0) & 0x0fff;
	}

	cunt = 0;
	temp = DRV_Reg32(TEMPMSR1) & 0x0fff;
	while (temp != 0xDA1 && cunt < 20) {
		cunt++;
		tscpu_dprintk("[Power/CPU_Thermal]1 temp=%d,cunt=%d\n", temp, cunt);
		temp = DRV_Reg32(TEMPMSR1) & 0x0fff;
	}

	cunt = 0;
	temp = DRV_Reg32(TEMPMSR2) & 0x0fff;
	while (temp != 0xDA1 && cunt < 20) {
		cunt++;
		tscpu_dprintk("[Power/CPU_Thermal]2 temp=%d,cunt=%d\n", temp, cunt);
		temp = DRV_Reg32(TEMPMSR2) & 0x0fff;
	}

	cunt = 0;
	temp = DRV_Reg32(TEMPMSR3) & 0x0fff;
	while (temp != 0xDA1 && cunt < 20) {
		cunt++;
		tscpu_dprintk("[Power/CPU_Thermal]3 temp=%d,cunt=%d\n", temp, cunt);
		temp = DRV_Reg32(TEMPMSR3) & 0x0fff;
	}


#if 0				/* debug */
	/* high temperature */
	tscpu_printk("===========================================\n");
	tscpu_printk("thermal_interrupt_trigger_test: high temp=0xD78\n");
	tscpu_printk("===========================================\n");
	temp = 0xD78;		/* set to very hot for pnp0 */
	DRV_WriteReg32(TEMPSPARE2, (0x00001000 + temp));	/* set sensor voltage and sensor valid */
	/* temp = 0x00001014;                        set to very hot for pnp0 */
	/* DRV_WriteReg32(TEMPSPARE2, temp);         set sensor voltage and sensor valid */



	count = 20;
	while (count--) {
		tscpu_printk("........");
		udelay(100);
	}

	count = 10;
	temp = DRV_Reg32(TEMPMSR0);
	while ((temp & 0x8000) == 0 && count--) {
		tscpu_printk("\n temp=%x\n", temp);
		temp = DRV_Reg32(TEMPMSR0);
	}
	/* first filter valid should be equal to 0x14 */
	if ((temp & 0x0FFF) != 0xD78) {
		tscpu_printk("thermal_interrupt_trigger_test: FAIL, read TEMPMSR0 = 0x%x\n",
				(temp & 0x0FFF));
	} else
		tscpu_printk("thermal_interrupt_trigger_test: PASS, read TEMPMSR0 = 0x%x\n",
				(temp & 0x0FFF));

#endif


#if 0				/* debug */

	count = 10;
	temp = DRV_Reg32(TEMPMSR0);
	/* tscpu_printk("temp=%x,TEMPMSR0=%x\n",temp,DRV_Reg32(TEMPMSR0)); */

	while (((temp & 0x0FFF) != 0xDA1) && (count--)) {
		temp = DRV_Reg32(TEMPMSR0);
		tscpu_printk("temp=%x,TEMPMSR0=%x, ", temp, DRV_Reg32(TEMPMSR0));
	}

	count = 10;
	temp1 = DRV_Reg32(TEMPMSR1);
	/* tscpu_printk("temp1=%x,TEMPMSR1=%x\n",temp1,DRV_Reg32(TEMPMSR1)); */

	while (((temp1 & 0x0FFF) != 0xDA1) && (count--)) {
		temp1 = DRV_Reg32(TEMPMSR1);
		tscpu_printk("temp1=%x,TEMPMSR1=%x, ", temp1, DRV_Reg32(TEMPMSR1));
	}

	count = 10;
	temp2 = DRV_Reg32(TEMPMSR2);
	/* tscpu_printk("temp2=%x,TEMPMSR2=%x\n",temp2,DRV_Reg32(TEMPMSR2)); */
	while (((temp2 & 0x0FFF) != 0xDA1) && (count--)) {
		temp2 = DRV_Reg32(TEMPMSR2);
		tscpu_printk("temp2=%x,TEMPMSR2=%x, ", temp2, DRV_Reg32(TEMPMSR2));
	}

	count = 10;
	temp3 = DRV_Reg32(TEMPMSR3);
	/* tscpu_printk("temp3=%x,TEMPMSR3=%x\n",temp2,DRV_Reg32(TEMPMSR3)); */
	while (((temp3 & 0x0FFF) != 0xDA1) && (count--)) {
		temp3 = DRV_Reg32(TEMPMSR3);
		tscpu_printk("temp3=%x,TEMPMSR3=%x\n", temp3, DRV_Reg32(TEMPMSR3));
	}


	tscpu_printk(KERN_CRIT
			"thermal_DBG  thermal_fast_init TEMPMSR0=%x,TEMPMSR1=%x,TEMPMSR2=%x,TEMPMSR3=%x\n",
			DRV_Reg32(TEMPMSR0), DRV_Reg32(TEMPMSR1), DRV_Reg32(TEMPMSR2), DRV_Reg32(TEMPMSR3));
#endif

	return 0;
}


#ifdef CONFIG_OF

static u64 of_get_phys_base(struct device_node *np)
{
	u64 size64;
	const __be32 *regaddr_p;

	regaddr_p = of_get_address(np, 0, &size64, NULL);
	if (!regaddr_p)
		return OF_BAD_ADDR;

	return of_translate_address(np, regaddr_p);
}

#if 0/*/3.10*/
static int get_io_reg_base(void)
{
	struct device_node *node = NULL;


	node = of_find_compatible_node(NULL, NULL, "mediatek,THERM_CTRL");
	BUG_ON(node == 0);
	if (node) {
		/* Setup IO addresses */
		thermal_base = of_iomap(node, 0);
		/* tscpu_printk("[THERM_CTRL] thermal_base=0x%x\n",thermal_base); */
	}
	of_property_read_u32(node, "reg", &thermal_phy_base);
	/* tscpu_printk("[THERM_CTRL] thermal_base thermal_phy_base=0x%x\n",thermal_phy_base); */



	/*get thermal irq num */
	thermal_irq_number = irq_of_parse_and_map(node, 0);
	/* tscpu_printk("[THERM_CTRL] thermal_irq_number=0x%x\n",thermal_irq_number); */
	if (!thermal_irq_number) {
		/* tscpu_printk("[THERM_CTRL] get irqnr failed=0x%x\n",thermal_irq_number); */
		return 0;
	}


	node = of_find_compatible_node(NULL, NULL, "mediatek,AUXADC");
	BUG_ON(node == 0);
	if (node) {
		/* Setup IO addresses */
		auxadc_ts_base = of_iomap(node, 0);
		/* tscpu_printk("[THERM_CTRL] auxadc_ts_base=0x%x\n",auxadc_ts_base); */
	}
	of_property_read_u32(node, "reg", &auxadc_ts_phy_base);
	/* tscpu_printk("[THERM_CTRL] auxadc_ts_phy_base=0x%x\n",auxadc_ts_phy_base); */



	node = of_find_compatible_node(NULL, NULL, "mediatek,INFRACFG_AO");
	BUG_ON(node == 0);
	if (node) {
		/* Setup IO addresses */
		INFRACFG_AO_base = of_iomap(node, 0);
		/* tscpu_printk("[THERM_CTRL] infracfg_ao_base=0x%x\n",infracfg_ao_base); */
	}


	node = of_find_compatible_node(NULL, NULL, "mediatek,APMIXED");
	BUG_ON(node == 0);
	if (node) {
		/* Setup IO addresses */
		apmixed_base = of_iomap(node, 0);
		/* tscpu_printk("[THERM_CTRL] apmixed_base=0x%x\n",apmixed_base); */
	}
	of_property_read_u32(node, "reg", &apmixed_phy_base);
	/* tscpu_printk("[THERM_CTRL] apmixed_phy_base=0x%x\n",apmixed_phy_base); */



	return 1;
}
#else
static int get_io_reg_base(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;

	if (node) {
		/* Setup IO addresses */
		thermal_base = of_iomap(node, 0);
		tscpu_printk("[THERM_CTRL] thermal_base=0x%lx\n", (unsigned long)thermal_base);

		/* get thermal phy base */
		thermal_phy_base = of_get_phys_base(node);
		tscpu_printk("[THERM_CTRL] thermal_phy_base=0x%lx\n", (unsigned long)thermal_phy_base);

		/* get thermal irq num */
		thermal_irq_number = irq_of_parse_and_map(node, 0);
		tscpu_printk("[THERM_CTRL] thermal_irq_number=0x%lx\n",
				(unsigned long)thermal_irq_number);
#if 0
		auxadc_ts_base = of_parse_phandle(node, "auxadc", 0);
		tscpu_printk("[THERM_CTRL] auxadc_ts_base=0x%lx\n", (unsigned long)auxadc_ts_base);

		auxadc_ts_phy_base = of_get_phys_base(auxadc_ts_base);
		tscpu_printk("[THERM_CTRL] auxadc_ts_phy_base=0x%lx\n",
				(unsigned long)auxadc_ts_phy_base);
#endif
		apmixed_base = of_parse_phandle(node, "apmixedsys", 0);
		tscpu_printk("[THERM_CTRL] apmixed_base=0x%lx\n", (unsigned long)apmixed_base);

		apmixed_phy_base = of_get_phys_base(apmixed_base);
		tscpu_printk("[THERM_CTRL] apmixed_phy_base=0x%lx\n", (unsigned long)apmixed_phy_base);

#if 0
		pericfg_base = of_parse_phandle(node, "pericfg", 0);
		tscpu_printk("[THERM_CTRL] pericfg_base=0x%lx\n", (unsigned long)pericfg_base);
#endif
	}

	node = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-auxadc");
	if (!node) {
		pr_err("[THERM_CTRL] find node failed\n");
		return -ENODEV;
	}

	auxadc_ts_base = of_iomap(node, 0);
	if (!auxadc_ts_base) {
		pr_err("[THERM_CTRL] get auxadc base failed\n");
		return -ENOMEM;
	}
	tscpu_printk("auxadc_base=0x%lx\n", (unsigned long)auxadc_ts_base);

	auxadc_ts_phy_base = of_get_phys_base(node);
		tscpu_printk("[THERM_CTRL] auxadc_ts_phy_base=0x%lx\n",
				(unsigned long)auxadc_ts_phy_base);

	node = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-infracfg");
	if (!node) {
		tscpu_printk("get INFRACFG_AO failed\n");
		return -ENODEV;
	}
	INFRACFG_AO_BASE = of_iomap(node, 0);
	if (!INFRACFG_AO_BASE) {
		tscpu_printk("INFRACFG_AO iomap failed\n");
		return -ENOMEM;
	}
	tscpu_printk("[THERM_CTRL] INFRACFG_AO_BASE=0x%lx\n", (unsigned long)INFRACFG_AO_BASE);

	return 1;
}
#endif /*/3.10*/

#endif

#ifdef CONFIG_OF
const long tscpu_dev_alloc_module_base_by_name(const char *name)
{
	unsigned long VA;
	struct device_node *node = NULL;

	node = of_find_compatible_node(NULL, NULL, name);
	if (!node) {
		tscpu_printk("find node failed\n");
		return 0;
	}
	VA = (unsigned long)of_iomap(node, 0);
	tscpu_printk("DEV: VA(%s): 0x%lx\n", name, VA);

	return VA;
}
#endif



static int tscpu_thermal_probe(struct platform_device *pdev)
/*static int __init tscpu_init(void)*/
{
	int err = 0;
	int temp = 0;
	int cnt = 0;


	struct proc_dir_entry *entry = NULL;
	struct proc_dir_entry *mtktscpu_dir = NULL;

	pr_debug("tscpu_thermal_probe\n");

	clk_peri_therm = devm_clk_get(&pdev->dev, "therm");
	BUG_ON(IS_ERR(clk_peri_therm));

	clk_auxadc = devm_clk_get(&pdev->dev, "auxadc");
	BUG_ON(IS_ERR(clk_auxadc));


#ifdef CONFIG_OF
	if (get_io_reg_base(pdev) == 0)
		return 0;
#endif

	thermal_cal_prepare();
	thermal_calibration();

	tscpu_thermal_clock_on();
	tscpu_reset_thermal();

	/*
	   TS_CON1 default is 0x30, this is buffer off
	   we should turn on this buffer berore we use thermal sensor,
	   or this buffer off will let TC read a very small value from auxadc
	   and this small value will trigger thermal reboot
	 */
	temp = DRV_Reg32(TS_CON1);
	pr_debug("tscpu_thermal_probe :TS_CON1=0x%x\n", temp);
	temp &= ~(0x00000030);	/* TS_CON1[5:4]=2'b00,   00: Buffer on, TSMCU to AUXADC */
	THERMAL_WRAP_WR32(temp, TS_CON1);	/* read abb need */
	udelay(200);
	/* RG_TS2AUXADC < set from 2'b11 to 2'b00 when resume.
	wait 100uS than turn on thermal controller. */

	/*BUG_ON((DRV_Reg32(TS_CON1) & 0x00000030) != 0x0);
	BUG_ON(clock_is_on(MT_CG_INFRA_AUXADC) == 0x0);*/

	/*add this function to read all temp first to avoid
	  write TEMPPROTTC first will issue an fake signal to RGU */
	tscpu_fast_initial_sw_workaround();


	while (cnt < 50) {
		temp = (DRV_Reg32(THAHBST0) >> 16);
		if (cnt > 10)
			tscpu_dprintk(KERN_CRIT "THAHBST0 = 0x%x,cnt=%d, %d\n", temp, cnt, __LINE__);
		if (temp == 0x0) {
			/* pause all periodoc temperature sensing point 0~2 */
			thermal_pause_all_periodoc_temp_sensing();	/* TEMPMSRCTL1 */
			break;
		}
		udelay(2);
		cnt++;
	}
	thermal_disable_all_periodoc_temp_sensing();	/* TEMPMONCTL0 */

	tscpu_dprintk(KERN_CRIT "cnt = %d, %d\n", cnt, __LINE__);

	/*Normal initial */
	thermal_initial_all_bank();

	thermal_release_all_periodoc_temp_sensing();	/* TEMPMSRCTL1 must release before start */

	read_all_bank_temperature();

	tscpu_update_temperature_timer_init();



#if MTK_TS_CPU_RT
	{
		tscpu_printk("mtktscpu_register_thermal creates kthermp\n");
		ktp_thread_handle = kthread_create(ktp_thread, (void *)NULL, "kthermp");
		if (IS_ERR(ktp_thread_handle)) {
			ktp_thread_handle = NULL;
			tscpu_printk("mtktscpu_register_thermal kthermp creation fails\n");
			goto err_unreg;
		}
		wake_up_process(ktp_thread_handle);
	}
#endif


#ifdef CONFIG_OF
	err =
		request_irq(thermal_irq_number, thermal_all_bank_interrupt_handler, IRQF_TRIGGER_LOW,
				THERMAL_NAME, NULL);
	if (err)
		tscpu_printk("tscpu_init IRQ register fail\n");
#else
	err =
		request_irq(THERM_CTRL_IRQ_BIT_ID, thermal_all_bank_interrupt_handler, IRQF_TRIGGER_LOW,
				THERMAL_NAME, NULL);
	if (err)
		tscpu_printk("tscpu_init IRQ register fail\n");
#endif

	tscpu_config_all_tc_hw_protect(trip_temp[0], tc_mid_trip);

	if (err) {
		tscpu_printk("thermal driver callback register failed..\n");
		return err;
	}

	err = init_cooler();
	if (err)
		return err;

	err = tscpu_register_DVFS_hotplug_cooler();
	if (err) {
		tscpu_printk("tscpu_register_DVFS_hotplug_cooler fail\n");
		return err;
	}
	err = tscpu_register_thermal();
	if (err) {
		tscpu_printk("tscpu_register_thermal fail\n");
		goto err_unreg;
	}


	mtktscpu_dir = mtk_thermal_get_proc_drv_therm_dir_entry();
	if (!mtktscpu_dir) {
		tscpu_printk("[%s]: mkdir /proc/driver/thermal failed\n", __func__);
	} else {
		entry =
			proc_create("tzcpu", S_IRUGO | S_IWUSR | S_IWGRP, mtktscpu_dir, &mtktscpu_fops);
		if (entry)
			proc_set_user(entry, uid, gid);

		entry = proc_create("tzcpu_log", S_IRUGO | S_IWUSR, mtktscpu_dir, &mtktscpu_log_fops);

		entry = proc_create("thermlmt", S_IRUGO, NULL, &mtktscpu_opp_fops);

		entry = proc_create("tzcpu_cal", S_IRUSR, mtktscpu_dir, &mtktscpu_cal_fops);

		entry =
			proc_create("tzcpu_read_temperature", S_IRUGO, mtktscpu_dir,
					&mtktscpu_read_temperature_fops);

		entry =
			proc_create("tzcpu_set_temperature", S_IRUGO | S_IWUSR, mtktscpu_dir,
					&mtktscpu_set_temperature_fops);

		entry =
			proc_create("tzcpu_talking_flag", S_IRUGO | S_IWUSR, mtktscpu_dir,
					&mtktscpu_talking_flag_fops);
#if CPT_ADAPTIVE_AP_COOLER
		entry =
			proc_create("clatm_setting", S_IRUGO | S_IWUSR | S_IWGRP, mtktscpu_dir,
					&mtktscpu_dtm_setting_fops);
		if (entry)
			proc_set_user(entry, uid, gid);
		entry =
			proc_create("clatm_gpu_threshold", S_IRUGO | S_IWUSR | S_IWGRP, mtktscpu_dir,
					&mtktscpu_gpu_threshold_fops);
		if (entry)
			proc_set_user(entry, uid, gid);
#endif				/* #if CPT_ADAPTIVE_AP_COOLER */

#if MTKTSCPU_FAST_POLLING
		entry =
			proc_create("tzcpu_fastpoll", S_IRUGO | S_IWUSR | S_IWGRP, mtktscpu_dir,
					&mtktscpu_fastpoll_fops);
		if (entry)
			proc_set_user(entry, uid, gid);
#endif				/* #if MTKTSCPU_FAST_POLLING */

		entry =
			proc_create("tzcpu_Tj_out_via_HW_pin", S_IRUGO | S_IWUSR, mtktscpu_dir,
					&mtktscpu_Tj_out_fops);
		if (entry)
			proc_set_user(entry, uid, gid);
#if THERMAL_GPIO_OUT_TOGGLE
		entry =
			proc_create("tzcpu_GPIO_out_monitor", S_IRUGO | S_IWUSR, mtktscpu_dir,
					&mtktscpu_GPIO_out_fops);
		if (entry)
			proc_set_user(entry, uid, gid);
#endif

		/* +ASC+ */
		entry = proc_create("clatm", S_IRUGO | S_IWUSR, mtktscpu_dir, &mtktscpu_atm_fops);
		if (entry)
			proc_set_user(entry, uid, gid);
		/* -ASC- */

#if THERMAL_HEADROOM
		entry = proc_create("clthp", S_IRUGO | S_IWUSR, mtktscpu_dir, &mtktscpu_thp_fops);
		if (entry)
			proc_set_user(entry, uid, gid);
#endif
	}

	return 0;


err_unreg:
	tscpu_unregister_DVFS_hotplug_cooler();
	return err;


}


static struct platform_driver mtk_thermal_driver = {
	.remove = NULL,
	.shutdown = NULL,
	.probe = tscpu_thermal_probe,
	.suspend = tscpu_thermal_suspend,
	.resume = tscpu_thermal_resume,
	.driver = {
		.name = THERMAL_NAME,
#ifdef CONFIG_OF
		.of_match_table = mt_thermal_of_match,
#endif
	},
};


static int __init tscpu_init(void)
{
	return platform_driver_register(&mtk_thermal_driver);
}

static void __exit tscpu_exit(void)
{

	tscpu_dprintk("tscpu_exit\n");

#if MTK_TS_CPU_RT
	if (ktp_thread_handle)
		kthread_stop(ktp_thread_handle);
#endif

	tscpu_unregister_thermal();
	tscpu_unregister_DVFS_hotplug_cooler();


	hrtimer_cancel(&ts_tempinfo_hrtimer);
	tscpu_thermal_clock_off();

#if THERMAL_DRV_UPDATE_TEMP_DIRECT_TO_MET
	mt_thermalsampler_registerCB(NULL);
#endif
}
module_init(tscpu_init);
module_exit(tscpu_exit);


/* late_initcall(thermal_late_init); */
/* device_initcall(thermal_late_init); */

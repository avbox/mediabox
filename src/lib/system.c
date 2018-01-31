#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif


#include "avbox.h"

EXPORT int
avbox_system_cpu_usage(void)
{
	FILE *f = NULL;
	int usage = 80;
	char buf[16];
	int64_t user, nic, sys, idle, iowait, irq, softirq,
		steal, guest, guest_nice;

	static int64_t last_actv = 0;
	static int64_t last_totl = 0;

	/* open /proc/stat and read the cpu line */
	if ((f = fopen("/proc/stat", "r")) == NULL) {
		LOG_VPRINT_ERROR("Could not open /proc/stat!: %s",
			strerror(errno));
		return usage;
	}
	if (fscanf(f, "%s %" PRIi64 " %" PRIi64 " %" PRIi64 " %" PRIi64 " %" PRIi64 " %" PRIi64 " %" PRIi64 " %" PRIi64 " %" PRIi64 " %" PRIi64,
		buf, &user, &nic, &sys, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice) <= 0) {
		LOG_VPRINT_ERROR("Could not fscanf() the cpu line: %s", strerror(errno));
		fclose(f);
		return usage;
	}
	fclose(f);

	const int64_t actv = user + nic + sys + irq + softirq + steal + guest + guest_nice;
	const int64_t totl = user + nic + sys + irq + softirq + steal + guest + guest_nice + idle + iowait;
	const int64_t actv_diff = actv - last_actv;
	const int64_t totl_diff = totl - last_totl;

	last_totl = totl;
	last_actv = actv;
	usage = (totl_diff == 0) ? 0 : (actv_diff * 100) / totl_diff;

	return usage;
}




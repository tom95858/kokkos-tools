#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <execinfo.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <algorithm>
#include <string>
#include <sys/time.h>
#include <cxxabi.h>
#include <unistd.h>
#include <curl/curl.h>
#include "kp_kernel_info.h"
#include "ldms_upload.h"

bool compareKernelPerformanceInfo(KernelPerformanceInfo* left, KernelPerformanceInfo* right) {
	return left->getTime() > right->getTime();
};

static uint64_t uniqID = 0;
static KernelPerformanceInfo* currentEntry;
static std::map<std::string, KernelPerformanceInfo*> count_map;
static double initTime;
static char* outputDelimiter;

#define MAX_STACK_SIZE 128

void increment_counter(const char* name, KernelExecutionType kType) {
	std::string nameStr(name);

	if(count_map.find(name) == count_map.end()) {
		KernelPerformanceInfo* info = new KernelPerformanceInfo(nameStr, kType);
		count_map.insert(std::pair<std::string, KernelPerformanceInfo*>(nameStr, info));

		currentEntry = info;
	} else {
		currentEntry = count_map[nameStr];
	}

	currentEntry->startTimer();
}

extern "C" void kokkosp_init_library(const int loadSeq,
	const uint64_t interfaceVer,
	const uint32_t devInfoCount,
	void* deviceInfo) {

	const char* output_delim_env = getenv("KOKKOSP_OUTPUT_DELIM");
	if(NULL == output_delim_env) {
		outputDelimiter = (char*) malloc(sizeof(char) * 2);
		sprintf(outputDelimiter, "%c", ' ');
	} else {
		outputDelimiter = (char*) malloc(sizeof(char) * (strlen(output_delim_env) + 1));
		sprintf(outputDelimiter, "%s", output_delim_env);
	}

	printf("KokkosP: LDMS JSON Connector Initialized (sequence is %d, version: %llu)\n", loadSeq, interfaceVer);

	initTime = seconds();
}

struct av_s {
	const char *av_name;
	char *av_value;
} av_dict[] = {
	{ "JOB_APP_ID" },
	{ "JOB_END" },
	{ "JOB_EXIT" },
	{ "JOB_ID" },
	{ "JOB_NAME" },
	{ "JOB_START" },
	{ "JOB_STATUS" },
	{ "JOB_USER" },
	{ "JOB_USER_ID" },
};
#define AV_DICT_LEN (sizeof(av_dict) / sizeof(av_dict[0]))

int av_cmp_fn(const void *a, const void *b)
{
	struct av_s *av = (struct av_s *)a;
	struct av_s *bv = (struct av_s *)b;
	return strcmp(av->av_name, bv->av_name);
}

static const char *job_data(const char *name, const char *default_value)
{
	struct av_s key;
	struct av_s *av;
	key.av_name = name;
	av = (struct av_s *)bsearch(&key, av_dict, AV_DICT_LEN, sizeof(*av), av_cmp_fn);
	if (av)
		return av->av_value;
	return default_value;
}

static void read_job_data()
{
	const char *job_file_path = getenv("LDMS_JOBINFO_DATA_FILE");
	if (!job_file_path)
		job_file_path = "/var/run/ldms_jobinfo.data";

	FILE* job_file = fopen(job_file_path, "r");
	if (!job_file)
		return;

	for (int i = 0; i < AV_DICT_LEN; i++) {
		if (av_dict[i].av_value)
			free(av_dict[i].av_value);
		av_dict[i].av_value = NULL;
	}

	char buf[80];
	char *s, *p;
	while (NULL != (s = fgets(buf, sizeof(buf), job_file))) {
		char *name = strtok_r(s, "=", &p);
		char *value = strtok_r(NULL, "=", &p);
		struct av_s key;
		key.av_name = name;
		struct av_s *av = (struct av_s *)bsearch(&key, av_dict,
							 AV_DICT_LEN, sizeof(*av),
							 av_cmp_fn);
		if (av) {
			/* skip leading \" */
			while (*value == '"')
				value++;
			/* Strip newlines */
			while (s = strstr(value, "\n"))
				*s = '\0';
			/* Strip trailing \" */
			while (s = strstr(value, "\""))
				*s = '\0';
			av->av_value = strdup(value);
		}
	}
}

extern "C" void kokkosp_finalize_library() {
	double finishTime = seconds();
	double kernelTimes = 0;

	char* mpi_rank = getenv("OMPI_COMM_WORLD_RANK");

	char* inst_data = getenv("LDMS_INSTANCE_DATA");
	char* upload_url = getenv("LDMS_UPLOAD_URL");
	printf("upload_url=%s\n", upload_url);
	char* hostname = (char*) malloc(sizeof(char) * 256);

	gethostname(hostname, 256);

	char* fileOutput = (char*) malloc(sizeof(char) * 256);
	sprintf(fileOutput, "%s-%d-%s.json", hostname, (int) getpid(),
		(NULL == mpi_rank) ? "0" : mpi_rank);

	free(hostname);
	FILE* output_data = fopen(fileOutput, "w");

	const double totalExecuteTime = (finishTime - initTime);
	std::vector<KernelPerformanceInfo*> kernelList;

	for(auto kernel_itr = count_map.begin(); kernel_itr != count_map.end(); kernel_itr++) {
		kernelList.push_back(kernel_itr->second);
		kernelTimes += kernel_itr->second->getTime();
	}

	std::sort(kernelList.begin(), kernelList.end(), compareKernelPerformanceInfo);

	read_job_data();

	fprintf(output_data, "{\"kokkos-kernel-data\" : {\n");
	fprintf(output_data, "    \"job-name\"               : \"%s\",\n",
		job_data("JOB_NAME", ""));
	fprintf(output_data, "    \"job-id\"                 : %s,\n",
		job_data("JOB_ID", "0"));
	fprintf(output_data, "    \"app-id\"                 : %s,\n",
		job_data("JOB_APP_ID", "0"));
	fprintf(output_data, "    \"user-id\"                : %s,\n",
		job_data("JOB_USER_ID", "0"));
	fprintf(output_data, "    \"start-time\"             : %s,\n",
		job_data("JOB_START", "0"));
	fprintf(output_data, "    \"inst-data\"              : \"%s\",\n",
		inst_data ? inst_data : job_data("JOB_NAME", ""));
	fprintf(output_data, "    \"mpi-rank\"               : %s,\n",
		(NULL == mpi_rank) ? "0" : mpi_rank);
	fprintf(output_data, "    \"total-app-time\"         : %.8f,\n", totalExecuteTime);
	fprintf(output_data, "    \"total-kernel-times\"     : %.8f,\n", kernelTimes);
	fprintf(output_data, "    \"total-non-kernel-times\" : %.8f,\n",
		(totalExecuteTime - kernelTimes));

	const double percentKokkos = (kernelTimes / totalExecuteTime) * 100.0;
	fprintf(output_data, "    \"percent-in-kernels\"     : %.2f,\n", percentKokkos);
	fprintf(output_data, "    \"unique-kernel-calls\"    : %lu,\n", (uint64_t) count_map.size());
	fprintf(output_data, "\n");

	fprintf(output_data, "    \"kernel-perf-info\"       : [\n");

	auto kernel_itr = count_map.begin();

	#define KERNEL_INFO_INDENT (char *)"       "

	if( kernel_itr != count_map.end() ) {
		kernel_itr->second->writeToFile(output_data, KERNEL_INFO_INDENT);

		for(; kernel_itr != count_map.end(); kernel_itr++) {
			fprintf(output_data, ",\n");
			kernel_itr->second->writeToFile(output_data, KERNEL_INFO_INDENT);
		}
	}

	fprintf(output_data, "\n");
	fprintf(output_data, "    ]\n");
	fprintf(output_data, "}}\n");
	fclose(output_data);
	if (upload_url) {
		ldms_upload(upload_url, fileOutput, 0);
	}
}

extern "C" void kokkosp_begin_parallel_for(const char* name, const uint32_t devID, uint64_t* kID) {
	*kID = uniqID++;

	if( (NULL == name) || (strcmp("", name) == 0) ) {
		fprintf(stderr, "Error: kernel is empty\n");
		exit(-1);
	}

	increment_counter(name, PARALLEL_FOR);
}

extern "C" void kokkosp_end_parallel_for(const uint64_t kID) {
	currentEntry->addFromTimer();
}

extern "C" void kokkosp_begin_parallel_scan(const char* name, const uint32_t devID, uint64_t* kID) {
	*kID = uniqID++;

	if( (NULL == name) || (strcmp("", name) == 0) ) {
		fprintf(stderr, "Error: kernel is empty\n");
		exit(-1);
	}

	increment_counter(name, PARALLEL_SCAN);
}

extern "C" void kokkosp_end_parallel_scan(const uint64_t kID) {
	currentEntry->addFromTimer();
}

extern "C" void kokkosp_begin_parallel_reduce(const char* name, const uint32_t devID, uint64_t* kID) {
	*kID = uniqID++;

	if( (NULL == name) || (strcmp("", name) == 0) ) {
		fprintf(stderr, "Error: kernel is empty\n");
		exit(-1);
	}

	increment_counter(name, PARALLEL_REDUCE);
}

extern "C" void kokkosp_end_parallel_reduce(const uint64_t kID) {
	currentEntry->addFromTimer();
}

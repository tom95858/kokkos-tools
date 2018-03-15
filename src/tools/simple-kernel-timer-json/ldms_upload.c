#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <sys/time.h>
#include <unistd.h>

#include <curl/curl.h>

struct input {
	FILE *in;
	size_t bytes_read; /* count up */
	CURL *hnd;
};

static size_t read_callback(void *ptr, size_t size, size_t nmemb, void *userp)
{
	struct input *i = (struct input *)userp;
	size_t retcode = fread(ptr, size, nmemb, i->in);
	i->bytes_read += retcode;
	return retcode;
}

extern "C" {
void ldms_upload(const char *url, const char *upload, int verbose)
{
	static struct input curl_input;
	FILE *out;
	int num = 0;
	struct stat file_info;
	curl_off_t uploadsize;
	CURL *curl = curl_easy_init();
	if (!curl)
		return;

	/* get the file size of the local file */
	stat(upload, &file_info);
	uploadsize = file_info.st_size;

	curl_input.in = fopen(upload, "r");
	curl_input.hnd = curl;

	curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
	curl_easy_setopt(curl, CURLOPT_READDATA, &curl_input);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, uploadsize);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

	if (verbose)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	/* we use a self-signed test server, skip verification during debugging */
	// curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	// curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

	curl_easy_perform(curl);
	curl_easy_cleanup(curl);
}
}
#ifdef LDMS_UPLOAD_CMD
#include <getopt.h>

static struct option long_opts[] = {
	{"url",     required_argument, 0,  'u' },
	{"file",    required_argument, 0,  'f' },
	{"verbose", no_argument,       0,  'v' },
	{0,         0,                 0,  0 }
};

void usage(int argc, char **argv)
{
	printf("usage: %s --file <filename> --url <http-url>\n",
	       argv[0]);
	exit(1);
}

static const char *short_opts = "u:f:v";

int main(int argc, char **argv)
{
	CURL *curl;
	char *url = NULL;
	char *filename = NULL;
	int opt, opt_idx;
	int verbose = 0;

	while ((opt = getopt_long(argc, argv,
				  short_opts, long_opts,
				  &opt_idx)) > 0) {
		switch (opt) {
		case 'u':
			url = strdup(optarg);
			break;
		case 'f':
			filename = strdup(optarg);
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage(argc, argv);
		}
	}
	if (!url || !filename)
		usage(argc, argv);

	ldms_upload(url, filename, verbose);

	return 0;
}
#endif

#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <regex.h>

#include <curl/curl.h>

#define SYSLOG_NAMES
#include <sys/syslog.h>

#include <systemd/sd-journal.h>
#include <systemd/sd-daemon.h>

#define LOCAL_SERVER_PORT 515
#define BUFFER_SIZE 1500
#define MESSAGE_SIZE 2048

#define return_defer(value) do { error_code = (value); goto defer; } while (0)

CURL *curl;
typedef struct {
	uint8_t priority;
	uint8_t facility;
	char * date;
	char * mid;
	char * pid;
	char * body;
} syslog_t;

/* prepare pattern matching outside loop*/
regex_t regex_rfc3164_date;
regex_t regex_rfc5424_date;

static regex_t regex_compile(const char * string) {
	regex_t preg;
	if (regcomp(&preg, string, REG_EXTENDED) != 0) {
		fprintf(stderr, "regcomp() failed using `%s`\n", string);
		regfree(&preg);
	}
	return preg;
}

static int regex_match(const char * string, const regex_t preg) {
	regoff_t length = 0;
	regmatch_t pmatch;
	if (regexec(&preg, string, 1, &pmatch, 0) == 0) {
		length = pmatch.rm_eo - pmatch.rm_so;
	}
	return length;
}

void process_syslog(char * buf, syslog_t * syslog) {
	// printf("%s\n", buf);

	if (sscanf(buf, "<%3" SCNu8 ">", &syslog->priority) > 0) {
		syslog->facility = LOG_FAC(syslog->priority);
		syslog->priority = LOG_PRI(syslog->priority);
	}
	else {
		syslog->facility = LOG_USER;
		syslog->priority = UINT8_MAX;
	}

	char * found = NULL;
	found = strchr(buf, '>');
	if (found) {
		syslog->date = found + 1;
	}
	else {
		syslog->date = buf;
	}

	int offset = 0;
	if (regex_rfc3164_date.__allocated) {
		offset = regex_match(syslog->date, regex_rfc3164_date);
	}
	if (offset == 0 && regex_rfc5424_date.__allocated) {
		offset = regex_match(syslog->date, regex_rfc5424_date);
	}
	if (offset > 0) {
		syslog->mid = syslog->date + offset;
		syslog->mid[0] = 0;
		++syslog->mid;
	}
	else {
		buf[19] = 0;
		syslog->mid = buf + 20;
	}


	/* parse priority from text (Mikrotik) */
	/* stolen from https://github.com/eworm-de/udp514-journal */
	char * match;
	if (syslog->priority == UINT8_MAX && (match = strndup(syslog->mid, BUFFER_SIZE)) != NULL) {
		CODE * pri;
		char * space = strchr(match, ' ');
		if (space != NULL)
			*space = 0;
		for (pri = prioritynames; pri->c_name && strstr(match, pri->c_name) == NULL; pri++);
		free(match);
		syslog->priority = (uint8_t)pri->c_val;
	}


	found = strchr(syslog->mid, ':');
	if (found) {
		syslog->body = found + 2;
		found[0] = 0;

		syslog->pid = "";
		found = strchr(syslog->mid, '[');
		if (found > syslog->mid) {
			syslog->pid = found + 1;
			found[0] = 0;
			char * found = strchr(syslog->pid, ']');
			found[0] = 0;
		}
	}
	else {
		syslog->body = syslog->mid;
	}

	// Some systems add a \n, some don't. Make this consistent.
	size_t body_last =  strlen(syslog->body) -1;
	if (syslog->body[body_last] == '\n') {
		syslog->body[body_last] = 0;
	}
}

int send_message(char * message) {
	// printf("%s", message); printf("%zu\n", strlen(message));
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, message);
	CURLcode result = curl_easy_perform(curl);
	if(result != CURLE_OK)
		fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(result));
	return (int)result;
}

/* Without this `curl_easy_perform()` outputs the reply to stdout. */
static size_t curl_discarding_write(char *data, size_t size, size_t nmemb, void *clientp)
{
	// printf("%s", data);
	(void) data; (void) size; (void) clientp;
	return nmemb;
}


int main(void) {
	int error_code = 0; // Assume no error.
	int sock = -1;


	sd_id128_t sdbootid;
	int ret = sd_id128_get_boot(&sdbootid);
	// TODO: What benefit is this over reading the file ourselves? (s/-//g)
	if (ret != 0){
		fprintf(stderr, "Error: Can not get bootid: %s\n", strerror(errno));
		return_defer(1);
		// TODO: Should this be a fatal error or should we continue with some default?
	}
	char bootid[33];
	snprintf(bootid, 33, SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(sdbootid));


	CURLcode result;
	result = curl_global_init(CURL_GLOBAL_ALL);
	if(result != CURLE_OK) {
		fprintf(stderr, "curl_global_init() failed: %s\n", curl_easy_strerror(result));
		return_defer((int)result);
	}
	curl = curl_easy_init();
	if(!curl) {
		fprintf(stderr, "curl_easy_init() failed.\n");
		return_defer(2);
	}

	/* Without this `curl_easy_perform()` outputs the reply to stdout. */
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discarding_write);

	curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
	curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:19532/");
	/* Check if systemd-journal-remote is listening on localhost ... */
	result = curl_easy_perform(curl);
	if(result != CURLE_OK) {
		long http_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		fprintf(stderr, "failed: [%ld] %s\n", http_code, curl_easy_strerror(result));
		return_defer((int)result);
	}

	/* Verify that is actuall is systemd-journal-remote listening ... */
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code != 406) {
		fprintf(stderr, "Error:"
				" systemd-journal-remote test point should return"
				" `HTTP/1.1 406 Not Acceptable`;"
				" We got response: %ld\n", http_code);
		return_defer((int)http_code);
	}

	/* Set URL to /upload for all future POSTs. */
	curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:19532/upload");


	/* Emulate systemd-journal-upload HTTP headers for future POSTs. */
	struct curl_slist *chunk = NULL;
	chunk = curl_slist_append(chunk, "User-Agent: systemd-journal-upload from syslog");
	chunk = curl_slist_append(chunk, "Accept: text/plain");
	chunk = curl_slist_append(chunk, "Content-Type: application/vnd.fdo.journal");
	chunk = curl_slist_append(chunk, "Transfer-Encoding: chunked");
	chunk = curl_slist_append(chunk, "Expect: 100-continue");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);


	int systemd_fds = sd_listen_fds(0);
	if (systemd_fds == 1) {
		// printf("Activating using socket passed from systemd.\n");
		sock = SD_LISTEN_FDS_START + 0;
	}
	else {
		struct sockaddr_storage listen = {0};

		/* getaddrinfo() is connecting to IPv4 first,
		 * we want more control so do this manually. */
		if ((sock = socket(AF_INET6, SOCK_DGRAM, 0)) > 0) {
			printf("Activating using AF_INET6 socket.\n");
			struct sockaddr_in6 * listen_addr = (struct sockaddr_in6 *)&listen;
			listen_addr->sin6_family = AF_INET6;
			listen_addr->sin6_addr = in6addr_any;
			listen_addr->sin6_port = htons(LOCAL_SERVER_PORT);
		}
		else if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) > 0) {
			printf("Activating using AF_INET socket.\n");
			struct sockaddr_in * listen_addr  = (struct sockaddr_in *)&listen;
			listen_addr->sin_family = AF_INET;
			listen_addr->sin_addr.s_addr = htonl(INADDR_ANY);
			listen_addr->sin_port = htons(LOCAL_SERVER_PORT);
		}
		else {
			fprintf(stderr, "could not open socket: %s\n", strerror(errno));
			return_defer(4);
		}

		if (bind(sock, (struct sockaddr*)&listen, sizeof(struct sockaddr_storage)) < 0) {
			fprintf(stderr, "could not bind on port %d: %s\n",
					LOCAL_SERVER_PORT, strerror(errno));
			return_defer(5);
		}
	}


	#define DATE_RFC3164 "^[A-Za-z]{3} [ 123][0-9] [ 012][0-9]:[0-5][0-9]:[0-6][0-9]"
	#define DATE_RFC5424 "^[1-9][0-9]{3}-[012][0-9]-[0123][0-9]T[012][0-9]:[0-5][0-9]:[0-6][0-9](.[0-9]{1,6})?(Z|[+-][012][0-9]:[0-5][0-9])"

	/* prepare pattern matching outside loop*/
	regex_rfc3164_date = regex_compile(DATE_RFC3164);
	regex_rfc5424_date = regex_compile(DATE_RFC5424);


	/* tell systemd we are ready to go... */
	sd_notify(0, "READY=1\nSTATUS=Listening for syslog input...");

	unsigned int count = 0;
	char message[MESSAGE_SIZE];

	/* server loop */
	while (1) {
		char buf[BUFFER_SIZE+1] = {0};

		struct sockaddr_storage client_storage = {0};
		struct sockaddr * client = (struct sockaddr *)&client_storage;

		socklen_t addrlen = sizeof(struct sockaddr_storage);
		if (recvfrom(sock, buf, BUFFER_SIZE, 0, client, &addrlen) < 0) {
			fprintf(stderr, "could not receive data: %s\n", strerror(errno));
			continue;
		}

		char hostnamestr[NI_MAXHOST] = {0};
		char * hostname = hostnamestr;
		getnameinfo(client, addrlen, hostnamestr, NI_MAXHOST, NULL, 0, NI_NOFQDN);
		if (client->sa_family == AF_INET6 && (strncmp("::ffff:", hostname, 7) == 0)) {
			hostname += 7;
		}


		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		long epoch = (ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);
		clock_gettime(CLOCK_MONOTONIC, &ts);
		long monotonic = (ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);

		syslog_t syslog = {0};
		process_syslog(buf, &syslog);

		/// TODO: Do we need a _CURSOR?
		/// TODO: Should we set the timestamps based on the date from sending syslog?
		/// TODO: Do we need _SOURCE_REALTIME_TIMESTAMP as well as SYSLOG_TIMESTAMP?
		snprintf(message, MESSAGE_SIZE,
				"__REALTIME_TIMESTAMP=%ld\n"
				"__MONOTONIC_TIMESTAMP=%ld\n"
				"_SOURCE_REALTIME_TIMESTAMP=%ld\n"
				"_BOOT_ID=%s\n"
				"_TRANSPORT=syslog\n"
				"PRIORITY=%d\n"
				"SYSLOG_FACILITY=%d\n"
				"SYSLOG_IDENTIFIER=%s\n"
				"SYSLOG_PID=%s\n"
				"_HOSTNAME=%s\n"
				"SYSLOG_TIMESTAMP=%s\n"
				"MESSAGE=%s\n"
				"\n",
				epoch,
				monotonic,
				epoch,
				bootid,
				syslog.priority,
				syslog.facility,
				syslog.mid,
				syslog.pid,
				hostname,
				syslog.date,
				syslog.body
					);

		send_message(message);

		/* count and update status */
		sd_notifyf(0, "READY=1\nSTATUS=Forwarded %d syslog messages.", ++count);
	}


defer:
	sd_notify(0, "STOPPING=1");
	close(sock);
	curl_easy_setopt(curl, CURLOPT_QUICK_EXIT, 1L); // The OS will handle the cleanup on `exit()` anyway.
	curl_easy_cleanup(curl);
	curl_global_cleanup();

	return error_code;
}

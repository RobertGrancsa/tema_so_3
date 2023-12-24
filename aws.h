/*
 * Asynchronous Web Server - header file (macros and structures)
 *
 * 2011-2017, Operating Systems
 */

#ifndef AWS_H_
#define AWS_H_		1

#ifdef __cplusplus
extern "C" {
#endif

#define AWS_LISTEN_PORT		8888
#define AWS_DOCUMENT_ROOT	"./"
#define AWS_REL_STATIC_FOLDER	"static/"
#define AWS_REL_DYNAMIC_FOLDER	"dynamic/"
#define AWS_ABS_STATIC_FOLDER	(AWS_DOCUMENT_ROOT AWS_REL_STATIC_FOLDER)
#define AWS_ABS_DYNAMIC_FOLDER	(AWS_DOCUMENT_ROOT AWS_REL_DYNAMIC_FOLDER)

typedef struct connection {
    int sockfd;
    char *path;
    char *buffer;
    int buffer_len;

    char *message;
    int message_len;

    int filefd;
    int filesize;
    int status;

    int iocb_num;
    struct iocb *iocb_read;
    struct iocb **piocb_read;
    struct iocb *iocb_write;
    struct iocb **piocb_write;
    char **iocb_buffer;
    int eventfd;
	io_context_t ctx;
    pthread_t thread_id;
	u_int64_t efd_val;
} connection_t;

#ifdef __cplusplus
}
#endif

#endif /* AWS_H_ */

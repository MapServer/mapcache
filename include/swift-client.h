/*
 * Based on https://github.com/ukyg9e5r6k7gubiekd6/swift-client
 */

#ifndef SWIFT_CLIENT_H_
#define SWIFT_CLIENT_H_

#include <stdio.h>
#include <wchar.h>
#include <curl/curl.h>

/**
 * High-level types of errors which can occur while attempting to use Swift.
 * More detail is available from lower-level libraries (such as curl)
 * using error callbacks specific to those libraries.
 */
enum swift_error {
	SCERR_SUCCESS       = 0, /* Success */
	SCERR_INIT_FAILED   = 1, /* Initialisation of this library failed */
	SCERR_INVARG        = 2, /* An invalid argument was supplied */
	SCERR_ALLOC_FAILED  = 3, /* Memory allocation failed */
	SCERR_URL_FAILED    = 4, /* Network operation on a URL failed */
	SCERR_FILEIO_FAILED = 5, /* I/O operation on a file failed */
	SCERR_AUTH_FAILED   = 6, /* Authentication failure */
	SCERR_NOT_FOUND     = 7, /* The resource was not found */
	SCERR_INVALID_REQ   = 8, /* An invalid request was sent */
	SCERR_SERVER_ERROR  = 9  /* The server errored */
};

/* Operations supported by Swift */
enum swift_operation {
	CREATE_CONTAINER       = 0,
	LIST_CONTAINER         = 1,
	SET_CONTAINER_METADATA = 2,
	DELETE_CONTAINER       = 3,
	PUT_OBJECT             = 4,
	GET_OBJECT             = 5,
	SET_OBJECT_METADATA    = 6,
	DELETE_OBJECT          = 7,
	HAS_OBJECT             = 8
};

/* A function which allocates, re-allocates or de-allocates memory */
typedef void *(*swift_allocator_func_t)(void *ptr, size_t newsize);

/* A function which receives POSIX errors which set errno */
typedef void (*errno_callback_t)(const char *funcname, int errno_val);

/* A function which receives curl errors */
typedef void (*curl_error_callback_t)(const char *curl_funcname, CURLcode res);

/* A function which receives libiconv errors */
// typedef void (*iconv_error_callback_t)(const char *iconv_funcname, int iconv_errno);

/* A function which supplies data from somewhere of its choice into memory upon demand */
typedef size_t (*supply_data_func_t)(void *ptr, size_t size, size_t nmemb, void *stream);

/* A function which receives data into somewhere of its choice from memory upon demand */
typedef size_t (*receive_data_func_t)(void *ptr, size_t size, size_t nmemb, void *userdata);

/* swift client library's per-thread private context */
struct swift_context_private {
	CURL *curl;       /* Handle to curl library's easy interface */
	// iconv_t iconv;    /* iconv library's conversion descriptor */
	unsigned int verify_cert_trusted;  /* True if the peer's certificate must chain to a trusted CA, false otherwise */
	unsigned int verify_cert_hostname; /* True if the peer's certificate's hostname must be correct, false otherwise */
	char *container;  /* Name of current container */
	char *object;     /* Name of current object */
	char *auth_token; /* Authentication token, usually previously obtained from Keystone */
	unsigned int base_url_len; /* Length of Swift base URL, with API version and account, but without container or object */
	char *base_url;   /* Swift base URL, with API version and account, but without container or object */
};

typedef struct swift_context_private swift_context_private_t;

/**
 * All use of this library is performed within a 'context'.
 * Contexts cannot be shared among threads; each thread must have its own context.
 * Your program is responsible for allocating and freeing context structures.
 * Contexts should be zeroed out prior to use.
 */
struct swift_context {

	/* These members are 'public'; your program may (and should) set them at will */

	/**
	 * Called when an error occurs with a POSIX API which sets 'errno'.
	 * Your program may set this function pointer in order to perform custom error handling.
	 * If this is NULL at the time swift_start is called, a default handler will be used.
	 */
	errno_callback_t errno_error;
	/**
	 * Called when a libcurl error occurs.
	 * Your program may set this function pointer in order to perform custom error handling.
	 * If this is NULL at the time swift_start is called, a default handler will be used.
	 */
	curl_error_callback_t curl_error;
	/**
	 * Called when a libiconv error occurs.
	 * Your program may set this function in order to perform custom error handling.
	 * If this is NULL at the time swift_start is called, a default handler will be used.
	 */
	// iconv_error_callback_t iconv_error;
	/**
	 * Called when this library needs to allocate, re-allocate or free memory.
	 * If size is zero and ptr is NULL, nothing is done.
	 * If size is zero and ptr is non-NULL, the previously-allocated memory at ptr is to be freed.
	 * If size is non-zero and ptr is NULL, memory of the given size is to be allocated.
	 * If size is non-zero and ptr is non-NULL, the previously-allocated memory at ptr
	 * is to be re-allocated to be the given size.
	 * If this function pointer is NULL at the time swift_start is called, a default re-allocator will be used.
	 */
	swift_allocator_func_t allocator;
	/* This member (and its members, recursively) are 'private'. */
	/* They should not be modified by your program unless you *really* know what you're doing. */
	swift_context_private_t pvt;
};

typedef struct swift_context swift_context_t;

/**
 * Begin using this library.
 * The context passed must be zeroed out, except for the public part,
 * in which you may want to over-ride the function pointers.
 * Function pointers left NULL will be given meaningful defaults.
 * This must be called early in the execution of your program,
 * before additional threads (if any) are created.
 * This must be called before any other use of this library by your program.
 * These restrictions are imposed by libcurl, and the libcurl restrictions are in turn
 * imposed by the libraries that libcurl uses.
 * If your program is a library, it will need to expose a similar API to,
 * and expose similar restrictions on, its users.
 */
enum swift_error swift_global_init(void);

/**
 * Cease using this library.
 * This must be called late in the execution of your program,
 * after all secondary threads (if any) have exited,
 * so that there is precisely one thread in your program at the time of the call.
 * This library must not be used by your program after this function is called.
 * This function must be called exactly once for each successful prior call to swift_global_init
 * by your program.
 * These restrictions are imposed by libcurl, and the libcurl restrictions are in turn
 * imposed by the libraries that libcurl uses.
 * If your program is a library, it will need to expose a similar API to,
 * and expose similar restrictions on, its users.
 */
void swift_global_cleanup(void);

/**
 * Begin using this library for a single thread of your program.
 * This must be called by each thread of your program in order to use this library.
 */
enum swift_error swift_start(swift_context_t *context);

/**
 * Cease using this library for a single thread.
 * This must be called by each thread of your program after it is finished using this library.
 * Each thread in your program must call this function precisely once for each successful prior call
 * to swift_start by that thread.
 * After this call, the context is invalid.
 */
void swift_end(swift_context_t *context);

/**
 * Control verbose logging to stderr of the actions of this library and the libraries it uses.
 * Currently this enables logging to standard error of libcurl's actions.
 */
enum swift_error swift_set_debug(swift_context_t *context, unsigned int enable_debugging);

/**
 * Control whether a proxy (eg HTTP or SOCKS) is used to access the Swift server.
 * Argument must be a URL, or NULL if no proxy is to be used.
 */
enum swift_error swift_set_proxy(swift_context_t *context, const char *proxy_url);

/**
 * Set the current Swift server URL. This must not contain any path information.
 */
enum swift_error swift_set_url(swift_context_t *context, const char *url);

/**
 * Control whether the Swift server should be accessed via HTTPS, or just HTTP.
 */
enum swift_error swift_set_ssl(swift_context_t *context, unsigned int use_ssl);

/**
 * Control whether an HTTPS server's certificate is required to chain to a trusted CA cert.
 */
enum swift_error swift_verify_cert_trusted(swift_context_t *context, unsigned int require_trusted_cert);

/**
 * Control whether an HTTPS server's hostname is required to match its certificate's hostname.
 */
enum swift_error swift_verify_cert_hostname(swift_context_t *context, unsigned int require_matching_hostname);

/**
 * Set the value of the authentication token to be supplied with requests.
 * This should have have been obtained previously from a separate authentication service.
 */
enum swift_error swift_set_auth_token(swift_context_t *context, const char *auth_token);

/**
 * Set the name of the current Swift container.
 */
enum swift_error swift_set_container(swift_context_t *context, char *container_name);

/**
 * Set the name of the current Swift object.
 */
enum swift_error swift_set_object(swift_context_t *context, char *object_name);

/**
 * Check the existence of an object.
 */
enum swift_error
swift_has(swift_context_t *context, int *exists);

/**
 * Retrieve an object from Swift and pass its data to the given callback function.
 */
enum swift_error swift_get(swift_context_t *context, receive_data_func_t receive_data_callback, void *callback_arg);

/**
 * Retrieve an object from Swift and place its data in the given-named file.
 */
enum swift_error swift_get_file(swift_context_t *context, const char *filename);

/**
 * Retrieve an object from Swift and place its data in the variable.
 */
enum swift_error swift_get_data(swift_context_t *context, size_t *size, void **data);

/**
 * Create a Swift container with the current container name.
 */
enum swift_error swift_create_container(swift_context_t *context, size_t metadata_count, const wchar_t **metadata_names, const wchar_t **metadata_values);

/**
 * Delete the Swift container with the current container name.
 */
enum swift_error swift_delete_container(swift_context_t *context);

/**
 * Insert or update an object in Swift using the data supplied by the given callback function.
 * The given callback_arg will be passed as the last argument to the callback function.
 * Optionally, also attach a set of metadata {name, value} tuples to the object.
 * metadata_count specifies the number of {name, value} tuples to be set. This may be zero.
 * If metadata_count is non-zero, metadata_names and metadata_values must be arrays, each of length metadata_count, specifying the {name, value} tuples.
 */
enum swift_error swift_put(swift_context_t *context, supply_data_func_t supply_data_callback, void *callback_arg);

/**
 * Insert or update an object in Swift using the data in the given-named file.
 * Optionally, also attach a set of metadata {name, value} tuples to the object.
 * metadata_count specifies the number of {name, value} tuples to be set. This may be zero.
 * If metadata_count is non-zero, metadata_names and metadata_values must be arrays, each of length metadata_count, specifying the {name, value} tuples.
 */
enum swift_error swift_put_file(swift_context_t *context, const char *filename);

/**
 * Insert or update an object in Swift using the size bytes of data located in memory at ptr.
 * Optionally, also attach a set of metadata {name, value} tuples to the object.
 * metadata_count specifies the number of {name, value} tuples to be set. This may be zero.
 * If metadata_count is non-zero, metadata_names and metadata_values must be arrays, each of length metadata_count, specifying the {name, value} tuples.
 */
enum swift_error swift_put_data(swift_context_t *context, const void *ptr, size_t size);

/**
 * Insert or update metadata for the current object.
 * metadata_count specifies the number of {name, value} tuples to be set.
 * metadata_names and metadata_values must be arrays, each of length metadata_count, specifying the {name, value} tuples.
 */
enum swift_error swift_set_metadata(swift_context_t *context, size_t metadata_count, const wchar_t **metadata_names, const wchar_t **metadata_values);

/**
 * Delete the Swift object with the current container and object names.
 */
enum swift_error swift_delete_object(swift_context_t *context);

#endif /* SWIFT_CLIENT_H_ */

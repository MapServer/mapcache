/*
 * Based on https://github.com/ukyg9e5r6k7gubiekd6/swift-client
 */

#include <assert.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "swift-client.h"

/**
 * The maximum length in bytes of a multi-byte UTF-8 sequence.
 *
 */
#define UTF8_SEQUENCE_MAXLEN 6

/* Prefix to be prepended to Swift metadata key names in order to generate HTTP headers */
#define SWIFT_METADATA_PREFIX "X-Object-Meta-"
/* Name of HTTP header used to pass authentication token to Swift server */
#define SWIFT_AUTH_HEADER_NAME "X-Auth-Token"

#ifdef min
#undef min
#endif
#define min(a, b) (((a) < (b)) ? (a) : (b))

/**
 * Default handler for POSIX errors which set errno.
 */
static void
default_errno_callback(const char *funcname, int errno_val)
{
	assert(funcname != NULL);
	errno = errno_val;
	perror(funcname);
}

/**
 * Default handler for libcurl errors.
 */
static void
default_curl_error_callback(const char *curl_funcname, CURLcode curl_err)
{
	assert(curl_funcname != NULL);
	fprintf(stderr, "%s failed: libcurl error code %ld: %s\n", curl_funcname, (long) curl_err, curl_easy_strerror(curl_err));
}

/**
 * Default memory [re-/de-]allocator.
 */
static void *
default_allocator(void *ptr, size_t size)
{
	if (0 == size) {
		if (ptr != NULL) {
			free(ptr);
		}
		return NULL;
	}
	if (NULL == ptr) {
		return malloc(size);
	}
	return realloc(ptr, size);
}

/**
 * To be called at start of user program, while still single-threaded.
 * Non-thread-safe and non-re-entrant.
 */
enum swift_error
swift_global_init(void)
{
	CURLcode curl_err;

	curl_err = curl_global_init(CURL_GLOBAL_ALL);
	if (curl_err != 0) {
		/* TODO: Output error indications about detected error in 'res' */
		return SCERR_INIT_FAILED;
	}

	return SCERR_SUCCESS;
}

/**
 * To be called at end of user program, while again single-threaded.
 * Non-thread-safe and non-re-entrant.
 */
void
swift_global_cleanup(void)
{
	curl_global_cleanup();
}

/**
 * To be called by each thread of user program that will use this library,
 * before first other use of this library.
 * Thread-safe and re-entrant.
 */
enum swift_error
swift_start(swift_context_t *context)
{
	assert(context != NULL);

	if (NULL == context->errno_error) {
		context->errno_error = default_errno_callback;
	}
	if (NULL == context->curl_error) {
		context->curl_error = default_curl_error_callback;
	}
	if (NULL == context->allocator) {
		context->allocator = default_allocator;
	}
	context->pvt.curl = curl_easy_init();
	if (NULL == context->pvt.curl) {
		/* NOTE: No error code from libcurl, so we assume/invent CURLE_FAILED_INIT */
		context->curl_error("curl_easy_init", CURLE_FAILED_INIT);
		return SCERR_INIT_FAILED;
	}

	return SCERR_SUCCESS;
}

/**
 * To be called by each thread of user program that will use this library,
 * after last other use of this library.
 * To be called once per successful call to swift_start by that thread.
 * Thread-safe and re-entrant.
 */
void
swift_end(swift_context_t *context)
{
	assert(context != NULL);

	curl_easy_cleanup(context->pvt.curl);
	context->pvt.curl = NULL;
	if (context->pvt.auth_token != NULL) {
		context->pvt.auth_token = context->allocator(context->pvt.auth_token, 0);
	}
	if (context->pvt.base_url != NULL) {
		context->pvt.base_url = context->allocator(context->pvt.base_url, 0);
	}
	if (context->pvt.container != NULL) {
		context->pvt.container = context->allocator(context->pvt.container, 0);
	}
	if (context->pvt.object != NULL) {
		context->pvt.object = context->allocator(context->pvt.object, 0);
	}
}

/**
 * Control whether a proxy (eg HTTP or SOCKS) is used to access the Swift server.
 * Argument must be a URL, or NULL if no proxy is to be used.
 */
enum swift_error
swift_set_proxy(swift_context_t *context, const char *proxy_url)
{
	CURLcode curl_err;

	assert(context != NULL);

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_PROXY, (NULL == proxy_url) ? "" : proxy_url);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_INVARG;
	}

	return SCERR_SUCCESS;
}

/**
 * Control verbose logging to stderr of the actions of this library and the libraries it uses.
 * Currently this enables logging to standard error of libcurl's actions.
 */
enum swift_error
swift_set_debug(swift_context_t *context, unsigned int enable_debugging)
{
	CURLcode curl_err;

	assert(context != NULL);

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_VERBOSE, enable_debugging ? 1 : 0);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_INVARG;
	}

	return SCERR_SUCCESS;
}

/**
 * Set the current Swift server URL. This must not contain any path information.
 */
enum swift_error
swift_set_url(swift_context_t *context, const char *url)
{
	size_t url_len;

	assert(context != NULL);

	url_len = strlen(url);
	context->pvt.base_url = context->allocator(context->pvt.base_url, url_len + 1 /* '\0' */);
	if (NULL == context->pvt.base_url) {
		return SCERR_ALLOC_FAILED;
	}
	strcpy(context->pvt.base_url, url);
	context->pvt.base_url_len = url_len;

	return SCERR_SUCCESS;
}

/**
 * Control whether an HTTPS server's certificate is required to chain to a trusted CA cert.
 */
enum swift_error
swift_verify_cert_trusted(swift_context_t *context, unsigned int require_trusted_cert)
{
	CURLcode curl_err;

	assert(context != NULL);

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_SSL_VERIFYPEER, (long) require_trusted_cert);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_URL_FAILED;
	}

	return SCERR_SUCCESS;
}

/**
 * Control whether an HTTPS server's hostname is required to match its certificate's hostname.
 */
enum swift_error
swift_verify_cert_hostname(swift_context_t *context, unsigned int require_matching_hostname)
{
	CURLcode curl_err;

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_SSL_VERIFYHOST, (long) require_matching_hostname);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_URL_FAILED;
	}

	return SCERR_SUCCESS;
}

/**
 * Set the value of the authentication token to be supplied with requests.
 * This should have been obtained previously from a separate authentication service.
 */
enum swift_error
swift_set_auth_token(swift_context_t *context, const char *auth_token)
{
	assert(context != NULL);
	assert(auth_token != NULL);

	context->pvt.auth_token = context->allocator(context->pvt.auth_token, strlen(auth_token) + 1 /* '\0' */);
	if (NULL == context->pvt.auth_token) {
		return SCERR_ALLOC_FAILED;
	}
	strcpy(context->pvt.auth_token, auth_token);

	return SCERR_SUCCESS;
}

/**
 * Set the name of the current Swift container.
 */
enum swift_error
swift_set_container(swift_context_t *context, char *container_name)
{
	assert(context != NULL);
	assert(container_name != NULL);
	context->pvt.container = context->allocator(context->pvt.container, strlen(container_name) + 1 /* '\0' */);
	if (NULL == context->pvt.container) {
		return SCERR_ALLOC_FAILED;
	}
	strcpy(context->pvt.container, container_name);

	return SCERR_SUCCESS;
}

/**
 * Set the name of the current Swift object.
 */
enum swift_error
swift_set_object(swift_context_t *context, char *object_name)
{
	assert(context != NULL);
	assert(object_name != NULL);
	context->pvt.object = context->allocator(context->pvt.object, strlen(object_name) + 1 /* '\0' */);
	if (NULL == context->pvt.object) {
		return SCERR_ALLOC_FAILED;
	}
	strcpy(context->pvt.object, object_name);

	return SCERR_SUCCESS;
}

/**
 * Generate a Swift URL from the current base URL, account, container and object.
 */
static enum swift_error
make_url(swift_context_t *context, enum swift_operation operation)
{
	size_t url_len = context->pvt.base_url_len;

	assert(context != NULL);
	assert(context->pvt.container != NULL);
	assert(context->pvt.base_url != NULL);
	assert(context->pvt.base_url_len);

	switch (operation) {
	case HAS_OBJECT:
	case PUT_OBJECT:
	case GET_OBJECT:
	case SET_OBJECT_METADATA:
	case DELETE_OBJECT:
		assert(context->pvt.object != NULL);
		url_len +=
			1 /* '/' */
			+ strlen(context->pvt.object)
		;
		/* no break: fall thru */
	case CREATE_CONTAINER:
	case LIST_CONTAINER:
	case SET_CONTAINER_METADATA:
	case DELETE_CONTAINER:
		url_len +=
			1 /* '/' */
			+ strlen(context->pvt.container)
		;
		break;
	default:
		assert(0);
		return SCERR_INVARG;
	}
	url_len++; /* '\0' */

	context->pvt.base_url = context->allocator(context->pvt.base_url, url_len);
	if (NULL == context->pvt.base_url) {
		return SCERR_ALLOC_FAILED;
	}

	switch (operation) {
	case CREATE_CONTAINER:
	case LIST_CONTAINER:
	case SET_CONTAINER_METADATA:
	case DELETE_CONTAINER:
		sprintf(
			context->pvt.base_url + context->pvt.base_url_len,
			"/%s",
			context->pvt.container
		);
		break;
	case HAS_OBJECT:
	case PUT_OBJECT:
	case GET_OBJECT:
	case SET_OBJECT_METADATA:
	case DELETE_OBJECT:
		sprintf(
			context->pvt.base_url + context->pvt.base_url_len,
			"/%s/%s",
			context->pvt.container,
			context->pvt.object
		);
		break;
	default:
		assert(0);
		return SCERR_INVARG;
	}

	return SCERR_SUCCESS;
}

/**
 * Execute a Swift request using the current protocol, hostname, API version, account, container and object,
 * and using the given HTTP method.
 * This is the portion of the request code that is common to all Swift API operations.
 * This function consumes headers.
 */
static enum swift_error
swift_request(swift_context_t *context, enum swift_operation operation, struct curl_slist *headers, supply_data_func_t produce_request_callback, void *produce_request_callback_arg, receive_data_func_t consume_response_callback, void *consume_response_callback_arg)
{
	CURLcode curl_err;
	enum swift_error sc_err;

	assert(context != NULL);

	sc_err = make_url(context, operation);
	if (sc_err != SCERR_SUCCESS) {
		return sc_err;
	}

	/* FIXME: Failed attempt to prevent libcurl from uselessly using chunked transfer encoding for empty request bodies */
	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_POSTFIELDS, NULL);
	if (CURLE_OK != curl_err) {
		return SCERR_URL_FAILED;
	}

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_POSTFIELDSIZE, 0);
	if (CURLE_OK != curl_err) {
		return SCERR_URL_FAILED;
	}

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_READFUNCTION, produce_request_callback);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_URL_FAILED;
	}

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_READDATA, produce_request_callback_arg);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_URL_FAILED;
	}

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_WRITEFUNCTION, consume_response_callback);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_URL_FAILED;
	}

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_WRITEDATA, consume_response_callback_arg);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_URL_FAILED;
	}

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_URL, context->pvt.base_url);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_URL_FAILED;
	}
	/* Set HTTP request method */
	{
		CURLoption curl_opt;
		union {
			long longval;
			const char *stringval;
		} curl_param;

		switch (operation) {
		case LIST_CONTAINER:
		case GET_OBJECT:
			/* method GET */
			curl_opt = CURLOPT_HTTPGET;
			curl_param.longval = 1L;
			break;
		case CREATE_CONTAINER:
		case PUT_OBJECT:
			/* method PUT */
			curl_opt = CURLOPT_UPLOAD; /* Causes libcurl to use HTTP PUT method */
			curl_param.longval = 1L;
			break;
		case SET_CONTAINER_METADATA:
		case SET_OBJECT_METADATA:
			/* method POST */
			curl_opt = CURLOPT_POST;
			curl_param.longval = 1L;
			break;
		case DELETE_CONTAINER:
		case DELETE_OBJECT:
			/* method DELETE */
			curl_opt = CURLOPT_CUSTOMREQUEST; /* Causes libcurl to use the given-named HTTP method */
			curl_param.stringval = "DELETE";
			break;
		case HAS_OBJECT:
			curl_opt = CURLOPT_NOBODY;
			curl_param.longval = 1L;
			break;
		default:
			/* Unrecognised Swift operation type */
			assert(0);
			return SCERR_INVARG;
		}
		curl_err = curl_easy_setopt(context->pvt.curl, curl_opt, curl_param);
		if (CURLE_OK != curl_err) {
			context->curl_error("curl_easy_setopt", curl_err);
			return SCERR_URL_FAILED;
		}
	}
	/* Append common headers to those requested by caller */
	{
		char *header = NULL;

		header = context->allocator(
			header,
			strlen(SWIFT_AUTH_HEADER_NAME)
			+ 2 /* ": " */
			+ strlen(context->pvt.auth_token)
			+ 1 /* '\0' */
		);
		if (NULL == header) {
			return SCERR_ALLOC_FAILED;
		}
		sprintf(header, SWIFT_AUTH_HEADER_NAME ": %s", context->pvt.auth_token);
		headers = curl_slist_append(headers, header);
		context->allocator(header, 0);
	}

	/* Append pseudo-header defeating libcurl's default addition of an "Expect: 100-continue" header. */
	headers = curl_slist_append(headers, "Expect:");

	curl_err = curl_easy_setopt(context->pvt.curl, CURLOPT_HTTPHEADER, headers);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_setopt", curl_err);
		return SCERR_URL_FAILED;
	}

	curl_err = curl_easy_perform(context->pvt.curl);
	if (CURLE_OK != curl_err) {
		context->curl_error("curl_easy_perform", curl_err);
		return SCERR_URL_FAILED;
	} else {
		long response_code;
		curl_easy_getinfo(context->pvt.curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (401 == response_code) {
			return SCERR_AUTH_FAILED;
		} else if (404 == response_code) {
			return SCERR_NOT_FOUND;
		} else if (response_code >= 400 && response_code < 500) {
			return SCERR_INVALID_REQ;
		} else if (response_code >= 500) {
			return SCERR_SERVER_ERROR;
		}
	}

	curl_slist_free_all(headers);

	return SCERR_SUCCESS;
}

/**
 * Null response consumer. Completely ignores the entire response.
 */
static size_t
ignore_response(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	return size * nmemb;
}

/**
 * Null request producer. Supplies a zero-length body.
 */
static size_t
empty_request(void *ptr, size_t size, size_t nmemb, void *arg)
{
	return 0;
}

/**
 * Check the existence of an object.
 */
enum swift_error
swift_has(swift_context_t *context, int *exists)
{
	enum swift_error err;
	assert(context != NULL);
	assert(context->pvt.auth_token != NULL);

	*exists = 0;

	err = swift_request(context, HAS_OBJECT, NULL, empty_request, NULL, NULL, NULL);

	if (err == SCERR_SUCCESS) {
		*exists = 1;
	}
	return err;
}

/**
 * Retrieve an object from Swift and pass its data to the given callback function.
 */
enum swift_error
swift_get(swift_context_t *context, receive_data_func_t receive_data_callback, void *callback_arg)
{
	assert(context != NULL);
	assert(context->pvt.auth_token != NULL);
	assert(receive_data_callback != NULL);

	return swift_request(context, GET_OBJECT, NULL, empty_request, NULL, receive_data_callback, callback_arg);
}

static size_t
write_data_to_file(void *ptr, size_t size, size_t nmemb, void *stream)
{
	assert(ptr != NULL);
	assert(stream != NULL);

	return fwrite(ptr, size, nmemb, (FILE *) stream);
}

/**
 * Retrieve an object from Swift and place its data in the given-named file.
 */
enum swift_error
swift_get_file(swift_context_t *context, const char *filename)
{
	FILE *stream;
	enum swift_error swift_err;

	assert(context != NULL);
	assert(filename != NULL);

	stream = fopen(filename, "wb");
	if (NULL == stream) {
		context->errno_error("fopen", errno);
		swift_err = SCERR_FILEIO_FAILED;
	} else {
		swift_err = swift_get(context, write_data_to_file, stream);
		if (fclose(stream) != 0) {
			if (SCERR_SUCCESS == swift_err) {
				swift_err = SCERR_FILEIO_FAILED;
			}
		}
	}

	return swift_err;
}

struct write_buffer {
	struct swift_context *context;
	char *ptr;
	size_t size;
};

static size_t
write_data_to_buffer(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct write_buffer *buffer = (struct write_buffer*) userdata;
	size_t bytesize = size * nmemb;

	buffer->ptr = buffer->context->allocator(buffer->ptr, buffer->size + bytesize + 1);
	memcpy(&(buffer->ptr[buffer->size]), ptr, bytesize);
	buffer->size += bytesize;
	buffer->ptr[buffer->size] = 0;
	return bytesize;
}

/**
 * Retrieve an object from Swift and place its data in the variable.
 */
enum swift_error
swift_get_data(swift_context_t *context, size_t *size, void **data)
{
	enum swift_error swift_err;
	struct write_buffer buffer;

	assert(context != NULL);
	assert(data != NULL);

	buffer.context = context;
	buffer.ptr = NULL;
	buffer.size = 0;

	swift_err = swift_get(context, write_data_to_buffer, &buffer);
	if (buffer.ptr == NULL) {
		if (SCERR_SUCCESS == swift_err) {
			swift_err = SCERR_FILEIO_FAILED;
		}
	}

	*size = buffer.size;
	*data = buffer.ptr;

	return swift_err;
}

/**
 * Create a Swift container with the current container name.
 */
enum swift_error
swift_create_container(swift_context_t *context, size_t metadata_count, const wchar_t **metadata_names, const wchar_t **metadata_values)
{
	assert(context != NULL);
	assert(context->pvt.auth_token != NULL);

	return swift_request(context, CREATE_CONTAINER, NULL, empty_request, NULL, ignore_response, NULL);
}

/**
 * Delete the Swift container with the current container name.
 */
enum swift_error
swift_delete_container(swift_context_t *context)
{
	assert(context != NULL);
	assert(context->pvt.auth_token != NULL);

	return swift_request(context, DELETE_CONTAINER, NULL, empty_request, NULL, ignore_response, NULL);
}

/**
 * Insert or update an object in Swift using the data supplied by the given callback function.
 * Optionally, also attach a set of metadata {name, value} tuples to the object.
 * metadata_count specifies the number of {name, value} tuples to be set. This may be zero.
 * If metadata_count is non-zero, metadata_names and metadata_values must be arrays, each of length metadata_count, specifying the {name, value} tuples.
 */
enum swift_error
swift_put(swift_context_t *context, supply_data_func_t supply_data_callback, void *callback_arg)
{
	struct curl_slist *headers = NULL;

	assert(context != NULL);
	assert(context->pvt.auth_token != NULL);

	return swift_request(context, PUT_OBJECT, headers, supply_data_callback, callback_arg, ignore_response, NULL);
}

static size_t
supply_data_from_file(void *ptr, size_t size, size_t nmemb, void *stream)
{
	assert(ptr != NULL);
	assert(stream != NULL);
	return fread(ptr, size, nmemb, (FILE *) stream);
}

/**
 * Insert or update an object in Swift using the data in the given-names file.
 * Optionally, also attach a set of metadata {name, value} tuples to the object.
 * metadata_count specifies the number of {name, value} tuples to be set. This may be zero.
 * If metadata_count is non-zero, metadata_names and metadata_values must be arrays, each of length metadata_count, specifying the {name, value} tuples.
 */
enum swift_error
swift_put_file(swift_context_t *context, const char *filename)
{
	FILE *stream;
	enum swift_error swift_err;

	assert(context != NULL);
	assert(filename != NULL);

	stream = fopen(filename, "rb");
	if (NULL == stream) {
		context->errno_error("fopen", errno);
		swift_err = SCERR_FILEIO_FAILED;
	} else {
		swift_err = swift_put(context, supply_data_from_file, stream);
		if (fclose(stream) != 0) {
			if (SCERR_SUCCESS == swift_err) {
				swift_err = SCERR_FILEIO_FAILED;
			}
		}
	}

	return swift_err;
}

struct data_from_mem_args {
	const unsigned char *ptr;
	size_t nleft;
};

static size_t
supply_data_from_memory(void *ptr, size_t size, size_t nmemb, void *cookie)
{
	struct data_from_mem_args *args = (struct data_from_mem_args *) cookie;
	size_t len = min(size * nmemb, args->nleft);

	assert(ptr != NULL);

	memcpy(ptr, args->ptr, len);
	args->ptr += len;
	args->nleft -= len;

	return len;
}

/**
 * Insert or update an object in Swift using the size bytes of data located in memory at ptr.
 * Optionally, also attach a set of metadata {name, value} tuples to the object.
 * metadata_count specifies the number of {name, value} tuples to be set. This may be zero.
 * If metadata_count is non-zero, metadata_names and metadata_values must be arrays, each of length metadata_count, specifying the {name, value} tuples.
 */
enum swift_error
swift_put_data(swift_context_t *context, const void *ptr, size_t size)
{
	struct data_from_mem_args args;

	assert(context != NULL);

	args.ptr = ptr;
	args.nleft = size;

	return swift_put(context, supply_data_from_memory, &args);
}

/**
 * Insert or update metadata for the current object.
 * metadata_count specifies the number of {name, value} tuples to be set.
 * metadata_names and metadata_values must be arrays, each of length metadata_count, specifying the {name, value} tuples.
 */
enum swift_error
swift_set_metadata(swift_context_t *context, size_t metadata_count, const wchar_t **metadata_names, const wchar_t **metadata_values)
{
	struct curl_slist *headers = NULL;

	assert(context != NULL);

	if (0 == metadata_count) {
		return SCERR_SUCCESS; /* Nothing to do */
	}

	return swift_request(context, SET_OBJECT_METADATA, headers, empty_request, NULL, ignore_response, NULL);
}

/**
 * Delete the Swift object with the current container and object names.
 */
enum swift_error
swift_delete_object(swift_context_t *context)
{
	assert(context != NULL);
	assert(context->pvt.auth_token != NULL);

	return swift_request(context, DELETE_OBJECT, NULL, empty_request, NULL, ignore_response, NULL);
}

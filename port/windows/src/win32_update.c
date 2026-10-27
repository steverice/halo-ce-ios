/*
WIN32_UPDATE.C

The Windows self-updater's system side (port/linux/src/update.h, in place of
posix_update.c): a download over HTTPS with WinHTTP, which checks the
server's certificate against Windows's own certificate store and follows
redirects, and the files of the running game replaced and the game started
again. A running executable and its loaded DLLs can be renamed, not
overwritten: the old files move aside first.

Paths are UTF-8, as SDL gives them.
*/

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

#include "update.h"
#include "posix.h"

#define UPDATE_USER_AGENT L"halo-ce-universal-updater"
#define TIMEOUT_MILLISECONDS 20000

/* (WinHTTP's TLS 1.3 flag, missing from older SDKs) */
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

static int wide_from_utf8(const char *text, wchar_t *wide, int size)
{
	return MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, size) > 0;
}

static void set_error(char *error, int error_size, const char *what)
{
	DWORD code = GetLastError();

	if (error && error_size > 0)
		snprintf(error, (size_t)error_size, "%s (error %lu)", what, (unsigned long)code);
}

int update_download(const char *url, const char *path, update_progress_proc progress, void *context, char *error,
	int error_size)
{
	wchar_t wide_url[2048], wide_path[MAX_PATH * 2], host[256], url_path[2048];
	URL_COMPONENTS components;
	HINTERNET session = NULL, connection = NULL, request = NULL;
	HANDLE file = INVALID_HANDLE_VALUE;
	DWORD status = 0, status_size = sizeof(status);
	DWORD length = 0, length_size = sizeof(length);
	DWORD protocols;
	unsigned long long received = 0;
	int succeeded = 0;

	if (!wide_from_utf8(url, wide_url, 2048) || !wide_from_utf8(path, wide_path, MAX_PATH * 2))
	{
		snprintf(error, (size_t)error_size, "a bad address or path");
		return 0;
	}
	memset(&components, 0, sizeof(components));
	components.dwStructSize = sizeof(components);
	components.lpszHostName = host;
	components.dwHostNameLength = 256;
	components.lpszUrlPath = url_path;
	components.dwUrlPathLength = 2048;
	if (!WinHttpCrackUrl(wide_url, 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS)
	{
		snprintf(error, (size_t)error_size, "not an https:// address: %s", url);
		return 0;
	}
	session = WinHttpOpen(UPDATE_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
	{
		set_error(error, error_size, "could not start WinHTTP");
		goto done;
	}
	/* (TLS 1.2 and 1.3; Windows versions without 1.3 take 1.2) */
	protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
	{
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
	}
	WinHttpSetTimeouts(session, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS,
		TIMEOUT_MILLISECONDS);
	connection = WinHttpConnect(session, host, components.nPort, 0);
	if (connection)
	{
		request = WinHttpOpenRequest(connection, L"GET", url_path, NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	}
	if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request, NULL))
	{
		set_error(error, error_size, "could not reach the server");
		goto done;
	}
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200)
	{
		snprintf(error, (size_t)error_size, "the server answered %lu", (unsigned long)status);
		goto done;
	}
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
	{
		length = 0;
	}
	file = CreateFileW(wide_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		set_error(error, error_size, "could not write the download");
		goto done;
	}
	for (;;)
	{
		char buffer[16384];
		DWORD count = 0, written = 0;

		if (!WinHttpReadData(request, buffer, sizeof(buffer), &count))
		{
			set_error(error, error_size, "the download broke off");
			goto done;
		}
		if (!count)
			break;
		if (!WriteFile(file, buffer, count, &written, NULL) || written != count)
		{
			set_error(error, error_size, "could not write the download");
			goto done;
		}
		received += count;
		if (progress)
			progress(context, received, length);
	}
	if (length && received != length)
	{
		snprintf(error, (size_t)error_size, "the download broke off");
		goto done;
	}
	succeeded = 1;

done:
	if (file != INVALID_HANDLE_VALUE)
	{
		CloseHandle(file);
		if (!succeeded)
			DeleteFileW(wide_path);
	}
	if (request)
		WinHttpCloseHandle(request);
	if (connection)
		WinHttpCloseHandle(connection);
	if (session)
		WinHttpCloseHandle(session);
	return succeeded;
}

/* ---------- the Delta list's requests (posix.h) */

/* Missing headers are empty; an oversized or unconvertible value fails rather
than letting a truncated signature or cache validator reach the caller. */
static int signed_header(HINTERNET request, const wchar_t *name, char *value, int size)
{
	wchar_t wide[512];
	DWORD bytes = sizeof(wide);

	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, wide, &bytes, WINHTTP_NO_HEADER_INDEX))
		return GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND;
	return WideCharToMultiByte(CP_UTF8, 0, wide, -1, value, size, NULL, NULL) > 0;
}

static int https_request(const char *url, const char *form, int form_size, const char *etag, char *response,
	int response_size, int timeout_milliseconds, struct posix_https_headers *headers, char *error, int error_size)
{
	wchar_t wide_url[2048], host[256], url_path[2048], conditional[160];
	URL_COMPONENTS components;
	HINTERNET session = NULL, connection = NULL, request = NULL;
	DWORD status = 0, status_size = sizeof(status);
	DWORD protocols, policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	int timeout = timeout_milliseconds > 0 ? timeout_milliseconds : TIMEOUT_MILLISECONDS;
	int received = 0, succeeded = 0;

	if (headers)
		memset(headers, 0, sizeof(*headers));
	if (response && response_size > 0)
		response[0] = 0;
	if (error && error_size > 0)
		error[0] = 0;
	if (!url || !response || !error || response_size < 2 || error_size < 2 || (form && form_size < 0) ||
		(etag && (strlen(etag) >= 128 || strpbrk(etag, "\r\n"))) ||
		!wide_from_utf8(url, wide_url, 2048))
	{
		return 0;
	}
	memset(&components, 0, sizeof(components));
	components.dwStructSize = sizeof(components);
	components.lpszHostName = host;
	components.dwHostNameLength = 256;
	components.lpszUrlPath = url_path;
	components.dwUrlPathLength = 2048;
	if (!WinHttpCrackUrl(wide_url, 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS)
	{
		snprintf(error, (size_t)error_size, "not an https:// address: %s", url);
		return 0;
	}
	session = WinHttpOpen(L"VISR", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
	{
		set_error(error, error_size, "could not start WinHTTP");
		goto done;
	}
	protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
	{
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
	}
	WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);
	connection = WinHttpConnect(session, host, components.nPort, 0);
	if (connection)
	{
		request = WinHttpOpenRequest(connection, form ? L"POST" : L"GET", url_path, NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	}
	if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)))
	{
		set_error(error, error_size, "could not disable redirects");
		goto done;
	}
	if (headers && !WinHttpAddRequestHeaders(request, L"Accept-Encoding: identity", (DWORD)-1L,
		WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
	{
		set_error(error, error_size, "could not set content encoding");
		goto done;
	}
	if (etag && *etag)
	{
		wcscpy(conditional, L"If-None-Match: ");
		if (!wide_from_utf8(etag, conditional + 15, 145) ||
			!WinHttpAddRequestHeaders(request, conditional, (DWORD)-1L,
				WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
		{
			set_error(error, error_size, "could not set cache validator");
			goto done;
		}
	}
	if (!request || !WinHttpSendRequest(request,
		form ? L"Content-Type: application/x-www-form-urlencoded" : WINHTTP_NO_ADDITIONAL_HEADERS,
		form ? (DWORD)-1L : 0, form ? (LPVOID)form : WINHTTP_NO_REQUEST_DATA, form ? (DWORD)form_size : 0,
		form ? (DWORD)form_size : 0, 0) || !WinHttpReceiveResponse(request, NULL))
	{
		set_error(error, error_size, "could not reach the server");
		goto done;
	}
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
	{
		set_error(error, error_size, "a broken answer");
		goto done;
	}
	if (headers)
	{
		char encoding[128] = "";

		if (!signed_header(request, L"Signature", headers->signature, sizeof(headers->signature)) ||
			!signed_header(request, L"Signature-Key", headers->signature_key, sizeof(headers->signature_key)) ||
			!signed_header(request, L"ETag", headers->etag, sizeof(headers->etag)) ||
			!signed_header(request, L"Retry-After", headers->retry_after, sizeof(headers->retry_after)) ||
			!signed_header(request, L"Content-Encoding", encoding, sizeof(encoding)))
		{
			snprintf(error, (size_t)error_size, "oversized or invalid response header");
			goto done;
		}
		if (*encoding && _stricmp(encoding, "identity"))
		{
			snprintf(error, (size_t)error_size, "unsupported content encoding");
			goto done;
		}
	}
	if (status == 200)
	{
		for (;;)
		{
			DWORD count = 0;

			if (received >= response_size - 1)
			{
				/* (a larger body: a failure, unless it ends here) */
				char extra;

				if (!WinHttpReadData(request, &extra, 1, &count) || count)
				{
					snprintf(error, (size_t)error_size, "the answer is too large");
					status = 0;
					goto done;
				}
				break;
			}
			if (!WinHttpReadData(request, response + received, (DWORD)(response_size - 1 - received), &count))
			{
				set_error(error, error_size, "the answer broke off");
				status = 0;
				goto done;
			}
			if (!count)
				break;
			received += (int)count;
		}
		response[received] = 0;
	}
	if (headers)
		headers->body_size = received;
	succeeded = 1;

done:
	if (!succeeded) response[0] = 0;
	if (request)
		WinHttpCloseHandle(request);
	if (connection)
		WinHttpCloseHandle(connection);
	if (session)
		WinHttpCloseHandle(session);
	return succeeded ? (int)status : 0;
}

int posix_https_request(const char *url, const char *form, int form_size, char *response, int response_size,
	int timeout_milliseconds, char *error, int error_size)
{
	return https_request(url, form, form_size, NULL, response, response_size, timeout_milliseconds, NULL,
		error, error_size);
}

int posix_https_get_signed(const char *url, const char *etag, char *response, int response_size,
	int timeout_milliseconds, struct posix_https_headers *headers, char *error, int error_size)
{
	if (!headers)
		return 0;
	return https_request(url, NULL, 0, etag, response, response_size, timeout_milliseconds, headers,
		error, error_size);
}

/* ---------- files and processes */

int update_executable_path(char *path, int size)
{
	wchar_t wide[MAX_PATH * 2];
	DWORD length = GetModuleFileNameW(NULL, wide, MAX_PATH * 2);

	return length > 0 && length < MAX_PATH * 2 &&
		WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, size, NULL, NULL) > 0;
}

int update_replace_file(const char *path, const char *new_path, const char *old_path)
{
	wchar_t wide_path[MAX_PATH * 2], wide_new[MAX_PATH * 2], wide_old[MAX_PATH * 2];
	int existed;

	if (!wide_from_utf8(path, wide_path, MAX_PATH * 2) || !wide_from_utf8(new_path, wide_new, MAX_PATH * 2) ||
		!wide_from_utf8(old_path, wide_old, MAX_PATH * 2))
	{
		return 0;
	}
	DeleteFileW(wide_old);
	existed = GetFileAttributesW(wide_path) != INVALID_FILE_ATTRIBUTES;
	if (existed && !MoveFileExW(wide_path, wide_old, MOVEFILE_REPLACE_EXISTING))
		return 0;
	if (!MoveFileExW(wide_new, wide_path, MOVEFILE_REPLACE_EXISTING))
	{
		if (existed)
			MoveFileExW(wide_old, wide_path, MOVEFILE_REPLACE_EXISTING);
		return 0;
	}
	return 1;
}

void update_delete_file(const char *path)
{
	wchar_t wide[MAX_PATH * 2];

	if (wide_from_utf8(path, wide, MAX_PATH * 2) && !DeleteFileW(wide))
		RemoveDirectoryW(wide);
}

int update_make_directory(const char *path)
{
	wchar_t wide[MAX_PATH * 2];

	return wide_from_utf8(path, wide, MAX_PATH * 2) &&
		(CreateDirectoryW(wide, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

int update_launch(const char *path)
{
	wchar_t wide[MAX_PATH * 2], command[MAX_PATH * 2 + 3], directory[MAX_PATH * 2];
	wchar_t *slash;
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;

	if (!wide_from_utf8(path, wide, MAX_PATH * 2))
		return 0;
	_snwprintf(command, MAX_PATH * 2 + 3, L"\"%ls\"", wide);
	wcscpy(directory, wide);
	slash = wcsrchr(directory, L'\\');
	if (slash)
		*slash = 0;
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	/* (none of this process's handles: its sockets hold the game's ports) */
	if (!CreateProcessW(wide, command, NULL, NULL, FALSE, 0, NULL, slash ? directory : NULL, &startup, &process))
		return 0;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return 1;
}

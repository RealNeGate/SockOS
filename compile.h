// 
// MIT License
// 
// Copyright (c) 2022 Yasser Arguelles Snape
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// 
// This is my "build script library"-inator for C
// It's inspired by nobuild but different
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_MEAN_AND_LEAN
#include "windows.h"

#define popen _popen
#define pclose _pclose
#define SLASH "\\"
#define PATH_MAX MAX_PATH
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define SLASH "/"
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define UNIX_STYLE 0
#else
#define UNIX_STYLE 1
#endif

static char command_buffer[4096];
static size_t command_length = 0;

inline static bool str_ends_with(const char* cstr, const char* postfix) {
    const size_t cstr_len = strlen(cstr);
    const size_t postfix_len = strlen(postfix);
	
    return postfix_len <= cstr_len && strcmp(cstr + cstr_len - postfix_len, postfix) == 0;
}

inline static const char* str_no_ext(const char* path) {
	size_t n = strlen(path);
    while (n > 0 && path[n - 1] != '.') {
        n -= 1;
    }
	
    if (n > 0) {
        char* result = malloc(n);
        memcpy(result, path, n);
        result[n - 1] = '\0';
		
        return result;
    } else {
        return path;
    }
}

#ifdef _WIN32
inline static void create_dir_if_not_exists(const char* path) {
	if (CreateDirectory(path, NULL)) {
		if (GetLastError() == ERROR_PATH_NOT_FOUND) {
			printf("Could not create directory '%s'\n", path);
			abort();
		}
	}
}
#else
inline static void create_dir_if_not_exists(const char* path) {
	mkdir(path, 0666);
}
#endif

#ifdef _WIN32
// https://bobobobo.wordpress.com/2009/02/02/getlasterror-and-getlasterrorasstring/
inline static LPSTR GetLastErrorAsString(void) {
	LPSTR buf = NULL;
	
	int result = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, 
							   0, GetLastError(), 0, (LPSTR)&buf, 0, 0);
	
	return buf;
}

typedef struct FileIter {
    HANDLE find_handle;
    WIN32_FIND_DATA find_data;
} FileIter;

inline static FileIter file_iter_open(const char* directory) {
    char buffer[PATH_MAX];
    snprintf(buffer, PATH_MAX, "%s\\*", directory);
	
	FileIter iter = { 0 };
    iter.find_handle = FindFirstFile(buffer, &iter.find_data);
	if (iter.find_handle == INVALID_HANDLE_VALUE) {
		printf("File iterator failed to open!!!\n");
		abort();
	}
	
	return iter;
}

inline static char* file_iter_next(FileIter* iter) {
	if(!FindNextFile(iter->find_handle, &iter->find_data)) {
		if (GetLastError() != ERROR_NO_MORE_FILES) {
			printf("File iterator failed to iterate!!!\n");
			abort();
		}
		
		return NULL;
	}
	
	return iter->find_data.cFileName;
}

inline static void file_iter_close(FileIter* iter) {
    if (!FindClose(iter->find_handle)) {
		printf("File iterator failed to close!!!\n");
		abort();
	}
}
#else
typedef struct FileIter {
    DIR* dir;
} FileIter;

inline static FileIter file_iter_open(const char* directory) {
    return (FileIter){ opendir(directory) };
}

inline static char* file_iter_next(FileIter* iter) {
	struct dirent* dp = readdir(iter->dir);
	return dp ? dp->d_name : NULL;
}

inline static void file_iter_close(FileIter* iter) {
    closedir(iter->dir);
}
#endif

inline static void cmd_append(const char* str) {
	size_t l = strlen(str);
	assert(command_length + l + 1 < sizeof(command_buffer));
	
	memcpy(&command_buffer[command_length], str, l + 1);
	command_length += l;
}

// return the child process stdout
inline static FILE* cmd_run() {
	FILE* file = popen(command_buffer, "r");
	if (file == NULL) {
		printf("command failed to execute! %s (%s)\n", command_buffer, strerror(errno));
		abort();
	}
	
	command_buffer[0] = 0;
	command_length = 0;
	return file;
}

// Print out whatever was on that file stream
inline static int cmd_dump(FILE* stream) {
	char buffer[4096];
	while (fread(buffer, sizeof(buffer), sizeof(char), stream)) {
		printf("%s", buffer);
	}
	return pclose(stream);
}

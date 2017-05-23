/*
   Copyright (C) 2013 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <windows.h>
#include <shlobj.h>
#define __STDC_FORMAT_MACROS
#define __USE_MINGW_ANSI_STDIO 1

// compiler specific definitions
#ifdef _MSC_VER // compiling with Visual Studio
#define PRIu64 "I64u"
#else // compiling with mingw
#include <inttypes.h>
#endif // compiler specific definitions

#include <stdio.h>
#include <spice/macros.h>

#include "file_xfer.h"
#include "as_user.h"

void FileXfer::reset()
{
    FileXferTasks::iterator iter;
    FileXferTask* task;

    for (iter = _tasks.begin(); iter != _tasks.end(); iter++) {
        task = iter->second;
        task->cancel();
        delete task;
    }
    _tasks.clear();
}

FileXfer::~FileXfer()
{
    reset();
}

uint32_t FileXfer::create_status_message(VDAgentFileXferStatusMessage** status,
                                     uint32_t id, uint32_t xfer_status,
                                     uint8_t* data, uint32_t data_size)
{
    *status = (VDAgentFileXferStatusMessage *) malloc(sizeof(VDAgentFileXferStatusMessage) + data_size);
    (*status)->id = id;
    (*status)->result = xfer_status;

    if (data)
        memcpy((*status)->data, data, data_size);

    return sizeof(VDAgentFileXferStatusMessage) + data_size;
}

uint32_t FileXfer::handle_start(VDAgentFileXferStartMessage* start,
                            VDAgentFileXferStatusMessage** status)
{
    char* file_meta = (char*)start->data;
    TCHAR file_path[MAX_PATH];
    char file_name[MAX_PATH];
    ULARGE_INTEGER free_bytes;
    FileXferTask* task;
    uint64_t file_size;
    HANDLE handle;
    AsUser as_user;
    int wlen;

    if (!g_key_get_string(file_meta, "vdagent-file-xfer", "name", file_name, sizeof(file_name)) ||
            !g_key_get_uint64(file_meta, "vdagent-file-xfer", "size", &file_size)) {
        vd_printf("file id %u meta parsing failed", start->id);
        return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    }
    vd_printf("%u %s (%" PRIu64 ")", start->id, file_name, file_size);
    if (!as_user.begin()) {
        vd_printf("as_user failed");
        return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    }
    if (FAILED(SHGetFolderPath(NULL, CSIDL_DESKTOPDIRECTORY | CSIDL_FLAG_CREATE, NULL,
            SHGFP_TYPE_CURRENT, file_path))) {
        vd_printf("failed getting desktop path");
        return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    }
    if (!GetDiskFreeSpaceEx(file_path, &free_bytes, NULL, NULL)) {
        vd_printf("failed getting disk free space %lu", GetLastError());
        return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    }
    if (free_bytes.QuadPart < file_size) {
        vd_printf("insufficient disk space %" PRIu64, free_bytes.QuadPart);
        return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_NOT_ENOUGH_SPACE,
                                     (uint8_t *)&(free_bytes.QuadPart), sizeof(uint64_t));
    }

    wlen = _tcslen(file_path);
    // make sure we have enough space
    // (1 char for separator, 1 char for filename and 1 char for NUL terminator)
    if (wlen + 3 >= MAX_PATH) {
        vd_printf("error: file too long %ls\\%s", file_path, file_name);
        return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    }

    file_path[wlen++] = TEXT('\\');
    file_path[wlen] = TEXT('\0');

    const int MAX_ATTEMPTS = 64; // matches behavior of linux vdagent
    const size_t POSTFIX_LEN = 6; // up to 2 digits in parentheses and final NULL: " (xx)"
    char *extension = strrchr(file_name, '.');
    if (!extension)
        extension = strchr(file_name, 0);

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        char dest_filename[SPICE_N_ELEMENTS(file_name) + POSTFIX_LEN];
        if (attempt == 0) {
            strcpy(dest_filename, file_name);
        } else {
            sprintf_s(dest_filename, SPICE_N_ELEMENTS(dest_filename),
                      "%.*s (%d)%s", int(extension - file_name), file_name, attempt, extension);
        }
        if ((MultiByteToWideChar(CP_UTF8, 0, dest_filename, -1, file_path + wlen, MAX_PATH - wlen)) == 0) {
            vd_printf("failed converting file_name:%s to WideChar", dest_filename);
            return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
        }
        handle = CreateFile(file_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }

        // If the file already exists, we can re-try with a new filename. If
        // it's a different error, there's not much we can do.
        if (GetLastError() != ERROR_FILE_EXISTS) {
            vd_printf("Failed creating %ls %lu", file_path, GetLastError());
            return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
        }
    }

    if (handle == INVALID_HANDLE_VALUE) {
        vd_printf("Failed creating %ls. More than 63 copies exist?", file_path);
        return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    }
    task = new FileXferTask(handle, file_size, file_path);
    _tasks[start->id] = task;
    return create_status_message(status, start->id, VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA, NULL, 0);
}

uint32_t FileXfer::handle_data(VDAgentFileXferDataMessage* data,
                           VDAgentFileXferStatusMessage** status)
{
    FileXferTasks::iterator iter;
    FileXferTask* task = NULL;
    DWORD written;
    uint32_t status_size = 0;

    iter = _tasks.find(data->id);
    if (iter == _tasks.end()) {
        vd_printf("file id %u not found", data->id);
        goto fin;
    }
    task = iter->second;
    task->pos += data->size;
    if (task->pos > task->size) {
        vd_printf("file xfer is longer than expected");
        goto fin;
    }
    if (!WriteFile(task->handle, data->data, (DWORD)data->size,
                   &written, NULL) || written != data->size) {
        vd_printf("file write failed %lu", GetLastError());
        goto fin;
    }
    if (task->pos < task->size) {
        return 0;
    }
    vd_printf("%u completed", iter->first);
    status_size = create_status_message(status, data->id, VD_AGENT_FILE_XFER_STATUS_SUCCESS, NULL, 0);

fin:
    if (task) {
        CloseHandle(task->handle);
        if (status_size > 0 && (*status)->result != VD_AGENT_FILE_XFER_STATUS_SUCCESS) {
            DeleteFile(task->name);
        }
        _tasks.erase(iter);
        delete task;
    }

    if (status_size > 0)
        return status_size;
    else
        return create_status_message(status, data->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
}

void FileXferTask::cancel()
{
    CloseHandle(handle);
    DeleteFile(name);
}

void FileXfer::handle_status(VDAgentFileXferStatusMessage* status)
{
    FileXferTasks::iterator iter;
    FileXferTask* task;

    vd_printf("id %u result %u", status->id, status->result);
    if (status->result != VD_AGENT_FILE_XFER_STATUS_CANCELLED) {
        vd_printf("only cancel is permitted");
        return;
    }
    iter = _tasks.find(status->id);
    if (iter == _tasks.end()) {
        vd_printf("file id %u not found", status->id);
        return;
    }
    task = iter->second;
    task->cancel();
    _tasks.erase(iter);
    delete task;
}

uint32_t FileXfer::dispatch(VDAgentMessage* msg, VDAgentFileXferStatusMessage** status)
{
    switch (msg->type) {
    case VD_AGENT_FILE_XFER_START:
        return handle_start((VDAgentFileXferStartMessage*)msg->data, status);
    case VD_AGENT_FILE_XFER_DATA:
        return handle_data((VDAgentFileXferDataMessage*)msg->data, status);
    case VD_AGENT_FILE_XFER_STATUS:
        handle_status((VDAgentFileXferStatusMessage*)msg->data);
        return 0;
    default:
        vd_printf("unsupported message type %u size %u", msg->type, msg->size);
    }
    return 0;
}

//minimal parsers for GKeyFile, supporting only key=value with no spaces.
#define G_KEY_MAX_LEN 256

bool FileXfer::g_key_get_string(char* data, const char* group, const char* key, char* value,
                                                  unsigned vsize)
{
    char group_pfx[G_KEY_MAX_LEN], key_pfx[G_KEY_MAX_LEN];
    char *group_pos, *key_pos, *next_group_pos, *start, *end;
    size_t len;

    snprintf(group_pfx, sizeof(group_pfx), "[%s]", group);
    if (!(group_pos = strstr((char*)data, group_pfx))) return false;

    snprintf(key_pfx, sizeof(key_pfx), "\n%s=", key);
    if (!(key_pos = strstr(group_pos, key_pfx))) return false;

    next_group_pos = strstr(group_pos + strlen(group_pfx), "\n[");
    if (next_group_pos && key_pos > next_group_pos) return false;

    start = key_pos + strlen(key_pfx);
    end = strchr(start, '\n');
    if (!end) return false;

    len = end - start;
    if (len >= vsize) return false;

    memcpy(value, start, len);
    value[len] = '\0';

    return true;
}

bool FileXfer::g_key_get_uint64(char* data, const char* group, const char* key, uint64_t* value)
{
    char str[G_KEY_MAX_LEN];

    if (!g_key_get_string(data, group, key, str, sizeof(str)))
        return false;
    return !!sscanf(str, "%" PRIu64, value);
}

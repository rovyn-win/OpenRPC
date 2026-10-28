#ifndef OPENRPC_INI_PARSER_H
#define OPENRPC_INI_PARSER_H
#include "stdafx.h"
HRESULT MountPath(const char* drive, const char* device) {
    STRING deviceName, linkName;
    char destination[MAX_PATH];
    int n = _snprintf(destination, sizeof(destination),
        KeGetCurrentProcessType() == 2 ? "\\System??\\%s" : "\\??\\%s", drive);
    if (n < 0 || n >= sizeof(destination)) return E_INVALIDARG;
    RtlInitAnsiString(&deviceName, device);
    RtlInitAnsiString(&linkName, destination);
    ObDeleteSymbolicLink(&linkName);
    return (HRESULT)ObCreateSymbolicLink(&linkName, &deviceName);
}
#endif

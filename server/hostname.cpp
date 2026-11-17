#include "hostname.h"

#include <windows.h>

std::string wivrn::hostname()
{
	char buffer[256];
	DWORD size = sizeof(buffer);
	if (GetComputerNameA(buffer, &size))
		return std::string(buffer, size);
	return "no-hostname";
}

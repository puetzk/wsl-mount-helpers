//Simple helper decoding findfs-style tags to the corresponding args for wsl --mount
// except that it allows you to specify <Disk> by using the linux blkid-style tags
// note that linux filesystem-level LABEL= or UUID= cannot be used, since windows cannot see these
// only two possibilities are supported:
// PTUUID=<uuid>
// PARTUUID=<uuid>
//   This also adds --partition <Index>
//   unless using you gave --bare, in which case it selects the physical device containing this partition
//
// Unlike wsl.exe, this executabler is manifested as requireAdministrator, and so it will automatically trigger UAC elevation if called from within wsl
// If called as wsl-mount-findfs.exe --mount|--unmount <tag> [options ...] , it will automatically call wsl.exe --mount|--unmount within that elevation,
// replacing the tag with its resolved form and appending any other options
// giving that elevation to wsl.exe

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <windows.h>
#include <setupapi.h>
#include <process.h>
#include "IntegrityLevel.h"

void ReportLastError(const char *caption, ...)
{
	char *messageBuffer;
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	               NULL, GetLastError(), GetUserDefaultUILanguage(), (LPSTR)&messageBuffer, 0, NULL);

	fputs("*** ", stderr);
	va_list args;
	va_start(args, caption);
	vfprintf(stderr, caption, args);
	va_end(args);
	fputs(": ", stderr);
	fputs(messageBuffer, stderr);
	LocalFree(messageBuffer);
}

typedef void (*ENUMDEVICEHANDLE_CALLBACK)(LPCTSTR, HANDLE, LPVOID);

// https://stackoverflow.com/questions/327718/how-to-list-physical-disks
void EnumClassDevHandles(REFGUID ClassGuid, ENUMDEVICEHANDLE_CALLBACK callback, LPVOID context)
{
	HDEVINFO hDiskClassDevices = SetupDiGetClassDevs(ClassGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	if(hDiskClassDevices == INVALID_HANDLE_VALUE) {
		ReportLastError("SetupDiGetClassDevs");
		return;
	}

	DWORD deviceIndex = 0;
	SP_DEVICE_INTERFACE_DATA DeviceInterfaceData = { .cbSize = sizeof(SP_DEVICE_INTERFACE_DATA) };

	while(SetupDiEnumDeviceInterfaces(hDiskClassDevices, NULL, ClassGuid, deviceIndex++, &DeviceInterfaceData))
	{
		PSP_DEVICE_INTERFACE_DETAIL_DATA DeviceInterfaceDetailData = NULL;
		DWORD DeviceInterfaceDetailDataSize = 0;
		BOOL success = SetupDiGetDeviceInterfaceDetail(hDiskClassDevices, &DeviceInterfaceData, DeviceInterfaceDetailData, DeviceInterfaceDetailDataSize, &DeviceInterfaceDetailDataSize, NULL);


		if(!success && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
			DeviceInterfaceDetailData = malloc(DeviceInterfaceDetailDataSize);

			ZeroMemory(DeviceInterfaceDetailData, DeviceInterfaceDetailDataSize);
			DeviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
			success = SetupDiGetDeviceInterfaceDetail(hDiskClassDevices, &DeviceInterfaceData, DeviceInterfaceDetailData, DeviceInterfaceDetailDataSize, &DeviceInterfaceDetailDataSize, NULL);
		}

		if(success && DeviceInterfaceDetailData) {
			HANDLE hDevice = CreateFile(DeviceInterfaceDetailData->DevicePath, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if(hDevice == INVALID_HANDLE_VALUE) {
				ReportLastError("CreateFile(%ls)", DeviceInterfaceDetailData->DevicePath);
			} else {

				(*callback)(DeviceInterfaceDetailData->DevicePath, hDevice, context);
				CloseHandle(hDevice);
			}
		} else {
			ReportLastError("SetupDiGetDeviceInterfaceDetail");
		}

		free(DeviceInterfaceDetailData);
	}

	if(GetLastError() != ERROR_NO_MORE_ITEMS) ReportLastError("SetupDiEnumDeviceInterfaces");

	SetupDiDestroyDeviceInfoList(hDiskClassDevices);
}

// https://learn.microsoft.com/en-us/windows/win32/devio/calling-deviceiocontrol
PDRIVE_LAYOUT_INFORMATION_EX GetDriveLayoutInformationEx(HANDLE hDevice)
{
	DWORD drive_layout_size = sizeof(DRIVE_LAYOUT_INFORMATION_EX);
	PDRIVE_LAYOUT_INFORMATION_EX drive_layout = malloc(drive_layout_size);
	DWORD bytesReturned;
	BOOL success = DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, drive_layout, drive_layout_size, &bytesReturned, NULL);
	while(!success && (GetLastError() == ERROR_INSUFFICIENT_BUFFER || GetLastError() == ERROR_MORE_DATA)) {
		/* Annoyingly, this call does not return how much space it needs' assume that we need to add room for another partition since that's the variable-sized array */
		drive_layout = realloc(drive_layout, drive_layout_size += sizeof(PARTITION_INFORMATION_EX));
		ZeroMemory(drive_layout, drive_layout_size);
		success = DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, drive_layout, drive_layout_size, &bytesReturned, NULL);
	}

	if(!success) {
		ReportLastError("IOCTL_DISK_GET_DRIVE_LAYOUT_EX");
		free(drive_layout);
		drive_layout = NULL;
	}

	return drive_layout;
}

char * GetPhysicalDrive(LPCTSTR name, HANDLE hDevice) {
	STORAGE_DEVICE_NUMBER device_number = {0};
	DWORD bytesReturned;
	if(!DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &device_number, sizeof(device_number), &bytesReturned, NULL)) {
		ReportLastError("IOCTL_STORAGE_GET_DEVICE_NUMBER");
	}
	
	// apparently the \\.\PhysicalDrive<n> part is something you just construct - even Old New Thing just did so rather than pull out some way to retrieve this string
	// https://devblogs.microsoft.com/oldnewthing/20201021-00/?p=104387 "How do I get from a volume to the physical disk that holds it?"
	if(device_number.DeviceType == FILE_DEVICE_DISK) {
		char PhysicalDrive[MAX_PATH];
		sprintf_s(PhysicalDrive, MAX_PATH, "\\\\.\\PhysicalDrive%u", device_number.DeviceNumber);
		return _strdup(PhysicalDrive);
	} else {
		fprintf(stderr, "*** %ls is not FILE_DEVICE_DISK, cannot map to \\\\.\\PhysicalDrive<n>", name);
		return NULL;
	}
}

bool parse_guid(const char *str, GUID *guid)
{
	return sscanf_s(str, "%08lx-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
	                &guid->Data1, &guid->Data2, &guid->Data3,
	                &guid->Data4[0], &guid->Data4[1], &guid->Data4[2], &guid->Data4[3], &guid->Data4[4], &guid->Data4[5], &guid->Data4[6], &guid->Data4[7]) == 11;
}

typedef char GUIDSTR[37];
const char * format_guid(GUIDSTR buf, REFGUID guid)
{
	sprintf_s(buf, sizeof(GUIDSTR), "%08lx-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
	          guid->Data1, guid->Data2, guid->Data3,
	          guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
	return buf;
}

void PrintPartitionInfo(LPCTSTR name, HANDLE hDevice, LPVOID context)
{
	printf("DevicePath = %ls\n", name);

	// TODO: could look up more things using IOCTL_STORAGE_QUERY_PROPERTY 
	// https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_storage_query_property
	// if we wanted to invent windows-specific tags, but I think the partition UUIDs will do for now
	char *PhysicalDrive = GetPhysicalDrive(name, hDevice);
	PDRIVE_LAYOUT_INFORMATION_EX drive_layout = GetDriveLayoutInformationEx(hDevice);

	if(drive_layout) {
		switch(drive_layout->PartitionStyle) {
			case PARTITION_STYLE_MBR:
				GUIDSTR guidstr;
				for(DWORD i = 0; i < drive_layout->PartitionCount; ++i) {
					if(drive_layout->PartitionEntry[i].Mbr.PartitionType != PARTITION_ENTRY_UNUSED) {
						// MBR does not actually store a GUID, but windows seems to construct one out of something, so allow searching for it
						printf("%s --partition %d PARTUUID=%s\n", PhysicalDrive, i, format_guid(guidstr, &drive_layout->PartitionEntry[i].Mbr.PartitionId));
					}
				}
				break;
			case PARTITION_STYLE_GPT:
				printf("%s PTUUID=%s\n", PhysicalDrive, format_guid(guidstr, &drive_layout->Gpt.DiskId));
				for(DWORD i = 0; i < drive_layout->PartitionCount; ++i) {
					printf("%s --partition %d PARTUUID=%s\n", PhysicalDrive, drive_layout->PartitionEntry[i].PartitionNumber, format_guid(guidstr, &drive_layout->PartitionEntry[i].Gpt.PartitionId));
				}
				break;
		}
	}
	free(PhysicalDrive);
	free(drive_layout);
}

struct FindGUIDContext
{
	GUID guid;
	const char *PhysicalDrive;
	DWORD PartitionNumber;
};

void MatchPTUUID(LPCTSTR name, HANDLE hDevice, struct FindGUIDContext *context)
{
	PDRIVE_LAYOUT_INFORMATION_EX drive_layout = GetDriveLayoutInformationEx(hDevice);

	if(drive_layout && IsEqualGUID(&context->guid, &drive_layout->Gpt.DiskId)) {
		context->PhysicalDrive = GetPhysicalDrive(name, hDevice);
	}
	free(drive_layout);
}

void MatchPARTUUID(LPCTSTR name, HANDLE hDevice, struct FindGUIDContext *context)
{
	PDRIVE_LAYOUT_INFORMATION_EX drive_layout = GetDriveLayoutInformationEx(hDevice);

	if(drive_layout) {
		switch(drive_layout->PartitionStyle) {
			case PARTITION_STYLE_MBR:
				for(DWORD i = 0; i < drive_layout->PartitionCount; ++i) {
					// MBR does not actually store a GUID, but windows seems to construct one out of something, so allow searching for it
					if(IsEqualGUID(&context->guid, &drive_layout->PartitionEntry[i].Mbr.PartitionId)) {
						context->PhysicalDrive = GetPhysicalDrive(name, hDevice);
						context->PartitionNumber = drive_layout->PartitionEntry[i].PartitionNumber;
					}
				}
				break;
			case PARTITION_STYLE_GPT:
				for(DWORD i = 0; i < drive_layout->PartitionCount; ++i) {
					if(IsEqualGUID(&context->guid, &drive_layout->PartitionEntry[i].Gpt.PartitionId)) {
						context->PhysicalDrive = GetPhysicalDrive(name, hDevice);
						context->PartitionNumber = drive_layout->PartitionEntry[i].PartitionNumber;
					}
				}
				break;
		}
	}
	free(drive_layout);
}

int main(int argc, char* argv[])
{
	#define MAX_TAG_ARGC 3 // <Device> [--partition <Index>]

	const char *wsl_device_argv[MAX_TAG_ARGC+1] = { NULL }; 
	int options_argindex;
	const char *tag;
	const char *mount = NULL;
	
	if(argc >= 3 && (!strcmp(argv[1], "--mount") || !strcmp(argv[1], "--unmount"))) {
		mount = argv[1];
		tag = argv[2];
		options_argindex = 3;
	} else if(argc >= 2) {
		tag = argv[1];
		options_argindex = 2;
	} else {
		fputs("wsl-mount-findfs.exe [--mount|--unmount] <PARTUUID|PTUUID=...> [Options...]\n", stderr);
		return 1;
	}

	// wsl --unmount actually detaches the whole disk and doesn't accept --partition
	bool bare = mount && !strcmp(mount, "--unmount");
	for(int i = options_argindex; i < argc; ++i) {
		if(!strcmp(argv[i], "--bare")) bare = true;
	}


	char PartitionNumber[11];

	if(!strcmp(tag, "--list")) {
		EnumClassDevHandles(&GUID_DEVINTERFACE_DISK, &PrintPartitionInfo, NULL);
	} else if(!strncmp(tag, "PARTUUID=", 9)) {
		struct FindGUIDContext context = { 0 };
		if(parse_guid(tag+9, &context.guid)) {
			EnumClassDevHandles(&GUID_DEVINTERFACE_DISK, &MatchPARTUUID, &context);
			wsl_device_argv[0] = context.PhysicalDrive;
			if(!bare) {
				wsl_device_argv[1] = "--partition";
				_ultoa_s(context.PartitionNumber, PartitionNumber, sizeof(PartitionNumber), 10);
				wsl_device_argv[2] = PartitionNumber;
			}
		} else {
			fprintf(stderr, "*** %s: invalid GUID\n", tag);
		}
	} else if(!strncmp(tag, "PTUUID=", 7)) {
		RunAsHighIntegrity();
		struct FindGUIDContext context = { 0 };
		if(parse_guid(tag+7, &context.guid)) {
			EnumClassDevHandles(&GUID_DEVINTERFACE_DISK, &MatchPTUUID, &context);
			wsl_device_argv[0] = context.PhysicalDrive;
		} else {
			fprintf(stderr, "*** %s: invalid GUID\n", tag);
		}
	} else {
		fputs("*** wsl-mount-findfs.exe: <Tag> must be from the GPT partition table (i.e. PARTUUID|PTUUID=...)\n"
		      "    Other linux blkid tags (e.g. UUID=, LABEL=, etc) are only parsed by linux\n"
		      "    and so cannot be used to locate the device to attach to wsl\n", stderr);
		return 1;
	}

	if(mount && wsl_device_argv[0]) {
		char wsl_exe[MAX_PATH];
		if(!SearchPathA(NULL, "wsl.exe", NULL, MAX_PATH, wsl_exe, NULL)) {
			strcpy_s(wsl_exe, sizeof(wsl_exe), "wsl.exe");
			fprintf(stderr, "*** wsl.exe not found by SearchPath()\n");
		}

		const char **wsl_argv = malloc(sizeof(char*) * (argc + MAX_TAG_ARGC));
		int wsl_argc = 0;
		wsl_argv[wsl_argc++] = wsl_exe;
		wsl_argv[wsl_argc++] = mount;

		for(int i = 0; wsl_device_argv[i]; ++i) {
			wsl_argv[wsl_argc++] = wsl_device_argv[i];
		}

		for(int i = options_argindex; i < argc; ++i) {
			wsl_argv[wsl_argc++] = argv[i];
		}
		wsl_argv[wsl_argc] = NULL;

		if(_execv(wsl_exe, wsl_argv) < 0) {
			ReportLastError("execv");
			// show failed command
			fputs("           ", stderr);
			for(int i = 0; wsl_argv[i]; ++i) {
				fputs(wsl_argv[i], stderr);
				fputc(' ', stderr);
			}
			fputc(L'\n', stderr);
		}
	} else {
		for(int i = 0; wsl_device_argv[i]; ++i) {
			if(i>0) fputc(' ', stdout);
			fputs(wsl_device_argv[i], stdout);
		}
		fputwc(L'\n', stdout);
	}
}

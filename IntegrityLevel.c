#include <stdbool.h>
#include <windows.h>
#include <psapi.h>

extern void ReportLastError(const char *caption, ...);

// https://devblogs.microsoft.com/oldnewthing/20221017-00/?p=107291 "How can I check the integrity level of my process?"
DWORD GetCurrentProcessIntegrityLevel()
{
	HANDLE hToken = GetCurrentProcessToken();

	DWORD IntegrityLevelSize = 0;
	TOKEN_MANDATORY_LABEL *pIntegrityLevel = NULL;
	bool success = GetTokenInformation(hToken,TokenIntegrityLevel,pIntegrityLevel,IntegrityLevelSize,&IntegrityLevelSize);
	if(!success && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
		pIntegrityLevel = malloc(IntegrityLevelSize);
		success = GetTokenInformation(hToken,TokenIntegrityLevel,pIntegrityLevel,IntegrityLevelSize,&IntegrityLevelSize);
	}

	if(!success) ReportLastError("GetTokenInformation");

	return *GetSidSubAuthority(pIntegrityLevel->Label.Sid, *GetSidSubAuthorityCount(pIntegrityLevel->Label.Sid)-1);
}

static void SkipArgv0(char **cmdline)
	{
	// note that the below handles ONLY the first argument (program name). Both parse_cmdline and CommandLineToArgvW handle this argument differently than the others
	// This is because the executable path must be a valid filename, so it can't contain certain some special characters (particularly ") that complicate later arguments
	// if extract is true, this also collapses any double-quoting and null-terminates the value, so that the skipped argument is available

	//http://stackoverflow.com/questions/9266467/get-executable-name-from-complete-command-line
	//http://msdn.microsoft.com/en-us/library/windows/desktop/bb776391(v=vs.85).aspx
	//http://support.microsoft.com/kb/q117258, http://cygwin.com/cygwin-ug-net/using-specialnames.html#pathnames-specialchars - cygwin (and various other things?) encode charcters not legal in NTFS filenames using private use unicode code points
	//http://windowsinspired.com/how-a-windows-programs-splits-its-command-line-into-individual-arguments/ (best, covers both CRT and shell32)

	char in_quote = 0;
	char c;
	char *in = *cmdline;
	for (; (c = *in); ++in) {
		if (c == '"') {
			in_quote = !in_quote;
		}
		else {
			if (!in_quote && (c == ' ' || c == '\t')) {
				break;
			}
		}
	}
	// skip delimiting whitespace
	while (*in == ' ' || *in == '\t') {
		++in;
	}
	*cmdline = in;
}

// this is a similar affect to /MANIFESTUAC:level=requireAdministrator
// (triggers UAC and unlocks admin powers)
// but can be triggered only on some code paths
//
// And more importantly, is compatible with WSL; the voodoo that creates a win32 process
// from a linux exec call apparently cannot trigger UAC and just gives "Permission denied",
// but it's OK if the win32 process (once running) does something that involves UAC
void RunAsHighIntegrity()
{
	if(GetCurrentProcessIntegrityLevel() < SECURITY_MANDATORY_HIGH_RID) {
		SHELLEXECUTEINFOA info = { .cbSize = sizeof(SHELLEXECUTEINFO) };
		info.fMask = SEE_MASK_NOCLOSEPROCESS;
		info.lpVerb = "runas";

		char current_exe[MAX_PATH];
		if(!GetModuleFileNameExA(GetCurrentProcess(),NULL,current_exe,MAX_PATH)) {
			ReportLastError("GetModuleFileNameEx");
		}
		info.lpFile = current_exe;
		info.lpParameters = GetCommandLineA(); // FIXME: need to strip off the first token (command)
		SkipArgv0(&info.lpParameters);

		//fprintf(stderr,"runas %s %s", info.lpFile, info.lpParameters);
		ShellExecuteExA(&info);

		WaitForSingleObject(info.hProcess,INFINITE);
		DWORD ExitCode;
		GetExitCodeProcess(info.hProcess,&ExitCode);
		CloseHandle(info.hProcess);
		ExitProcess(ExitCode);
	}
}

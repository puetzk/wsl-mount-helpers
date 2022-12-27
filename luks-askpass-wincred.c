#include <stdio.h>
#include <windows.h>
#include <tchar.h>
#include <wincred.h>

// simple "askpass" GUI helper allowing WSL to invoke a win32 process that uses the normal
// wincred UI prompt and session credential store to securely retain the LUKS passphrase,
// in case WSL idles and is shut down, then is needed again.
// https://learn.microsoft.com/en-us/windows/win32/api/wincred/

// very loosely inspired by git-credential-wincred.exe and git-credential-manager
// being used to have linux CLI commands use windows GUI prompts and logon session storage
// https://github.com/git/git/blob/master/contrib/credential/wincred/git-credential-wincred.c
// https://github.com/GitCredentialManager/git-credential-manager/blob/main/docs/wsl.md#how-it-works
// https://learn.microsoft.com/en-us/windows/wsl/filesystems#interoperability-between-windows-and-linux-commands

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

int _tmain(int argc, LPCTSTR argv[])
{
	if(argc >= 2) {
		TCHAR TargetName[MAX_PATH];
		_stprintf_s(TargetName, MAX_PATH, TEXT("LUKS\\%s"), argv[1]);
		PCREDENTIAL pCredential;

		DWORD dwAuthError = ERROR_SUCCESS;
		if(argc >= 3 && !_tcsncmp(argv[2], TEXT("--auth-error="), 12)) {
			// used after crypt_activate_* returned an error
			// to indicate that a previous attmpt failed and we should *not* just use the stored password
			dwAuthError = ERROR_INVALID_PASSWORD; // posix errno == EPERM, but just assume that's the only case (for now)
		}

		if((dwAuthError == ERROR_SUCCESS) && CredRead(TargetName, CRED_TYPE_GENERIC, 0, &pCredential)) {
			fwrite(pCredential->CredentialBlob, 1, pCredential->CredentialBlobSize, stdout);
		} else {
			fprintf(stderr, "*** %ls not found\n", TargetName);
			CREDUI_INFO UiInfo = { .cbSize = sizeof(CREDUI_INFO) };
			UiInfo.pszCaptionText = TEXT("LUKS cryptsetup");
			//UiInfo.pszMessageText = TEXT("password for ");
			//TODO:UiInfo.hbmBanner, 320x60 pixels

			ULONG ulAuthPackage;
			LPVOID pvOutAuthBuffer;
			ULONG ulOutAuthBufferSize;
			BOOL fSave = TRUE;

			BYTE *pPackedCredentials = NULL;
			DWORD cbPackedCredentials = 50;
			BOOL success = CredPackAuthenticationBuffer(CRED_PACK_GENERIC_CREDENTIALS, TargetName, TEXT(""), pPackedCredentials, &cbPackedCredentials);
			if(!success && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
				pPackedCredentials = malloc(cbPackedCredentials);
				success = CredPackAuthenticationBuffer(CRED_PACK_GENERIC_CREDENTIALS, TargetName, TEXT(""), pPackedCredentials, &cbPackedCredentials);
			}
			if(!success) ReportLastError("CredPackAuthenticationBuffer");

			switch(CredUIPromptForWindowsCredentials(&UiInfo, dwAuthError, &ulAuthPackage,
			                                         pPackedCredentials, cbPackedCredentials,
			                                         &pvOutAuthBuffer, &ulOutAuthBufferSize, &fSave, CREDUIWIN_CHECKBOX | CREDUIWIN_GENERIC | CREDUIWIN_IN_CRED_ONLY)) {
				case ERROR_SUCCESS:
					LPTSTR pszUsername = NULL;
					DWORD cchUsername = 0;
					LPTSTR pszDomainName = NULL;
					DWORD cchDomainName = 0;
					LPTSTR pszPassword = NULL;
					DWORD cchPassword = 0;
					success = CredUnPackAuthenticationBuffer(0, pvOutAuthBuffer, ulOutAuthBufferSize, pszUsername, &cchUsername, pszDomainName, &cchDomainName, pszPassword, &cchPassword);
					if(!success && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
						pszUsername = calloc(cchUsername, sizeof(TCHAR));
						pszDomainName = calloc(cchDomainName, sizeof(TCHAR));
						pszPassword = calloc(cchPassword, sizeof(TCHAR));
						success = CredUnPackAuthenticationBuffer(0, pvOutAuthBuffer, ulOutAuthBufferSize, pszUsername, &cchUsername, pszDomainName, &cchDomainName, pszPassword, &cchPassword);
					}
					if(!success) ReportLastError("CredUnPackAuthenticationBufferW");

					SecureZeroMemory(pvOutAuthBuffer, ulOutAuthBufferSize);
					CoTaskMemFree(pvOutAuthBuffer);

					BYTE *pu8Password = NULL;
					int cbu8Password = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS | WC_COMPOSITECHECK, pszPassword, cchPassword, NULL, 0, NULL, NULL);
					pu8Password = malloc(cbu8Password+1);
					WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS | WC_COMPOSITECHECK, pszPassword, cchPassword, pu8Password, cbu8Password, NULL, NULL);
					pu8Password[cbu8Password] = '\0';

					SecureZeroMemory(pszPassword, sizeof(TCHAR)*cchPassword);
					free(pszPassword);

					//_ftprintf(stderr, L"Username = %s, Domain = %s\n", pszUsername, pszDomainName);
					free(pszDomainName);
					free(pszUsername);

					fwrite(pu8Password, 1, cbu8Password, stdout);

					if(fSave) {
						CREDENTIAL cred;
						cred.Flags = 0;
						cred.Type = CRED_TYPE_GENERIC;
						cred.TargetName = TargetName;
						cred.Comment = TEXT("saved by luks-askpass-wincred");
						cred.CredentialBlobSize = cbu8Password;
						cred.CredentialBlob = pu8Password;
						cred.Persist = CRED_PERSIST_SESSION; //CRED_PERSIST_LOCAL_MACHINE, but I don't want it actually saved
						cred.AttributeCount = 0;
						cred.Attributes = NULL;
						cred.TargetAlias = NULL;
						cred.UserName = TargetName;

						if(!CredWrite(&cred, 0)) ReportLastError("CredWrite");
					}

					SecureZeroMemory(pu8Password, cbu8Password);
					free(pu8Password);

					break;
				case ERROR_CANCELLED:
					return 2;
				default:
					return 1;
			}
		}
	} else {
		fprintf(stderr, "usage: luks-askpass-wincred.exe <TargetName>\n");
		return 1;
	}
}

#include <stdio.h>

#include <set>
#include <vector>
#include <map>
#include <string>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>  

#include <Windows.h>
#include <TlHelp32.h>

#include "utils.h"
#include "md5.h"

using namespace std;


FILE* s_file_logger = NULL;
#define  my_print(...)  {printf(__VA_ARGS__);if(s_file_logger){fprintf(s_file_logger, __VA_ARGS__);}; }


static void sys_sleep(uint32_t nMilliseconds)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(nMilliseconds));
}

static uint64_t GetDLLBaseAddress(std::wstring needDllName, int32_t pid)
{
	if (pid == 0) return 0;
	MODULEENTRY32 module;
	module.dwSize = sizeof(module);
	HANDLE hProcessSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
	if (hProcessSnap == INVALID_HANDLE_VALUE) {
		uint32_t error_code = GetLastError();
		return 0;
	}

	uint64_t ret = 0;

	BOOL bMore = Module32First(hProcessSnap, &module);
	int loaded_count = 0;
	while (bMore) {
		std::wstring dll_name = module.szModule;
		dll_name = lower_case(dll_name);
		needDllName = lower_case(needDllName);
		if (dll_name == needDllName) {
			ret = (uint64_t)module.modBaseAddr;
			break;
		}
		bMore = Module32Next(hProcessSnap, &module);
	}
	CloseHandle(hProcessSnap);

	return ret;
}

static std::vector<DWORD> GetPidByProcessName(wstring process_name) 
{ 

	std::vector<DWORD> ret;

	HANDLE hSnapshot; 
	PROCESSENTRY32W lppe; 
	hSnapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	if (hSnapshot == NULL) 
		return ret; 

	lppe.dwSize = sizeof(lppe); 

	if (!::Process32FirstW(hSnapshot, &lppe))
		return ret;

	process_name = lower_case(process_name);

	do { 
		wstring exeName = lppe.szExeFile;
		exeName = lower_case(exeName);
		if(exeName == process_name) { 
			ret.push_back(lppe.th32ProcessID);
		}   
	} while (::Process32NextW(hSnapshot, &lppe));
	CloseHandle(hSnapshot);
	return ret;
}

static bool my_WriteProcessMemory(HANDLE h, uint64_t address, const uint8_t* byte, int byte_size)
{
	return WriteProcessMemory(h, (LPVOID)address, byte, byte_size, NULL);
}

#define  patch(i, b, b_size)  my_WriteProcessMemory(hopen, target_addr+i, b, b_size)

static std::wstring s_target_process_name = L"ChsIME.exe";

///< key = md5, value = address	|  certutil -hashfile FILE MD5
static std::map<std::string, uint64_t> s_support_files = {
	{"b3448bf077665f2e1ca67094bcf2a7c5", 0x14DE1},
	{"de5fa392a825332ab3e348ef0316b514", 0x16A61},
	{"f653c99d4a0c61d4b2c64358b8213bd8", 0x15C11},
	{"c8bc76c87563e78c9bc85ee9f4f96760", 0x15C11},
};
///< current file md5
static std::string s_file_md5;

class win10_wubi_patch
{
public:
	win10_wubi_patch() 
	{
		_exit = false;
	}
	~win10_wubi_patch()
	{
		_exit = true;
		if (_thread.joinable()) {
			_thread.join();
		}
	}
	void RunThread()
	{
		_start_time = GetTickCount();
		_thread = std::thread(std::bind(&win10_wubi_patch::Execute, this));
	}

	bool CheckExit()
	{
		bool bExit = false;
		do 
		{
			if (_patched_pids.size() <= 0) {
				break;
			}

			///< wait 10 seconds
			if (GetTickCount() - _start_time <= 10 * 1000) {
				break;
			}

			bExit = true;
		} while (0);
		return bExit;
	}
protected:
	virtual int32_t Execute(void)
	{
		uint32_t count = 0;
		while (!_exit) {

			sys_sleep(2500);

			std::vector<DWORD> pids = GetPidByProcessName(s_target_process_name);
			for (auto pid : pids) {
				if (_patched_pids.find(pid) == _patched_pids.end()) {
					count++;
					bool bOK = do_patch(pid);
					std::string s_bok = "success";
					if (bOK) {
						_patched_pids.insert(pid);
					} else {
						s_bok = "failed";
					}
					my_print("-- do patch, pid = %d, times = %d, result = %s\n", pid, count, s_bok.c_str());
				}
			}
		}
		return 0;
	}
private:
	bool do_patch(int32_t pid)
	{

		bool bOK = false;
		HANDLE hopen = NULL;
		do 
		{
			hopen = OpenProcess(PROCESS_ALL_ACCESS | PROCESS_TERMINATE | PROCESS_VM_OPERATION | PROCESS_VM_READ |
				PROCESS_VM_WRITE, FALSE, pid);
			if (hopen == NULL) {
				break;
			}

			uint64_t target_addr = GetDLLBaseAddress(s_target_process_name, pid);
			if (target_addr <= 0) {
				break;
			}

			///< patch here!
			uint8_t patch_bytes[] = {
				0x31, 0xC0
			};

			///< check again
			if (s_support_files.find(s_file_md5) == s_support_files.end()) {
				break;
			}
			uint64_t address = s_support_files[s_file_md5];
			bOK = patch(address, patch_bytes, sizeof(patch_bytes)); 
		} while (0);

		if (hopen) {
			CloseHandle(hopen);
			hopen = NULL;
		}

		return bOK;
	}
private:
	volatile bool _exit;
	std::set<DWORD> _patched_pids;
	std::thread _thread;
	DWORD _start_time;
};

bool check_support_version()
{
	std::string chs_ime_path = get_system_winodws_path();
	chs_ime_path += "System32\\InputMethod\\CHS\\ChsIME.exe";
	bool bSupport = false;
	do 
	{
		s_file_md5 = md5file(chs_ime_path.c_str());
		if (s_file_md5.empty()) {
			break;
		}

		s_file_md5 = lower_case(s_file_md5);
		if (s_support_files.find(s_file_md5) == s_support_files.end()) {
			break;
		}

		bSupport = true;
	} while (0);
	return bSupport;
}

static void Usage()
{
	my_print("if you want to exit when patch success, pass '--exit_when_patched' to launch command\n");
}

class file_keeper
{
public:
	file_keeper()
	{
		_file = NULL;
	}
	~file_keeper()
	{
		Close();
	}
public:
	bool Open(const std::string& filename)
	{
		Close();
		bool bOK = false;
		do 
		{
			FILE* file = fopen(filename.c_str(), "wb");
			if (!file) {
				break;
			}

			_file = file;
			bOK = true;
		} while (0);
		return bOK;
	}

	FILE* GetFile()
	{
		return _file;
	}

	void Close()
	{
		if (_file) {
			fclose(_file);
			_file = NULL;
		}
	}
private:
	FILE* _file;
};

int main(int argc, char* argv[])
{
	file_keeper logger;
	std::string app_data = getenv("appdata");
	app_data += "\\wubi_patch.log";
	logger.Open(app_data);
	s_file_logger = logger.GetFile();

	my_print("---Widows 10 Disable English Switch Key(Shift) For Wubi InputMethod---\n");
	my_print("key press [Q] to exit\n");
	Usage();
	my_print("\n");

	if (!check_support_version()) {
		my_print("file(ChsIME.exe) not support, need send ChsIME.exe to author.\n");
		getchar();
		return -1;
	}

	if (!set_debug_privilege()) {
		my_print("admin privileges are required.\n");
		return -1;
	}

	bool exit_when_patched = false;
	if (argc >= 2) {
		std::string param1 = argv[1];
		if (param1 == "--exit_when_patched") {
			exit_when_patched = true;
		}
	}

	term_init();

	win10_wubi_patch wubi_patch;
	wubi_patch.RunThread();
	do {
		if (read_key() == 'q') {
			my_print("exit(user)\n");
			sys_sleep(500);
			break;
		}

		if (exit_when_patched) {
			if (wubi_patch.CheckExit()) {
				my_print("exit(patched)\n");
				sys_sleep(500);
				break;
			}
		}

		sys_sleep(10);
	} while (1);

	term_exit();
	return 0;
}
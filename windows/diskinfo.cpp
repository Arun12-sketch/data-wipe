#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <memory>

std::vector<std::wstring> GetDiskInfo() {
  std::vector<std::wstring> diskInfoList;

  // Command to execute PowerShell command
  const wchar_t* cmd = L"powershell.exe -Command \"Get-PSDrive -PSProvider FileSystem | Format-Table Name,Free,Used\"";

  // Open pipe to run command
  FILE* pipe = _wpopen(cmd, L"rt");
  if (!pipe) {
    diskInfoList.push_back(L"Error opening pipe");
    return diskInfoList;
  }

  wchar_t buffer[256];
  while (fgetws(buffer, sizeof(buffer)/sizeof(wchar_t), pipe)) {
    diskInfoList.push_back(std::wstring(buffer));
  }
  _pclose(pipe);

  return diskInfoList;
}

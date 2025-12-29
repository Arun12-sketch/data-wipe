// // linux/my_application.cc
// #include "my_application.h"

// #include <flutter_linux/flutter_linux.h>
// #ifdef GDK_WINDOWING_X11
// #include <gdk/gdkx.h>
// #endif

// #include "flutter/generated_plugin_registrant.h"

// #include <cstdio>
// #include <cstdlib>
// #include <sstream>

// #include <cstdarg>
// #include <string>
// #include <vector>
// #include <cstring>
// #include <fstream>
// #include <random>
// #include <sys/stat.h>
// #include <thread>
// #include <chrono>
// #include <unistd.h>
// #include <fcntl.h>
// #include <sys/ioctl.h>
// #include <linux/fs.h>
// #include <errno.h>
// #include <tuple>
// #include <algorithm>
// #include <functional>
// #include <sys/wait.h>

// #ifndef APPLICATION_ID
// #define APPLICATION_ID "com.example.code_wipe"
// #endif

// struct _MyApplication {
//   GtkApplication parent_instance;
//   char** dart_entrypoint_arguments;
//   FlMethodChannel* method_channel;
// };

// G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION)

// // -------------------- Globals --------------------
// static FlMethodChannel* g_method_channel = nullptr;
// static bool g_wiping_active = false;

// // -------------------- Logging helper --------------------
// static void Log(const char* fmt, ...) {
//   va_list ap;
//   va_start(ap, fmt);
//   vprintf(fmt, ap);
//   printf("\n");
//   fflush(stdout);
//   va_end(ap);
// }

// // -------------------- Shell helper: run command quietly --------------------
// static bool RunCmdQuiet(const std::string& cmd) {
//   std::string full = cmd + " >/dev/null 2>&1";
//   int rc = system(full.c_str());
//   if (rc == -1) return false;
//   return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
// }

// // -------------------- Wait for device / partition stabilization --------------------
// static bool WaitForDeviceStable(const std::string& path, int max_tries = 30, int wait_ms = 300) {
//   for (int i = 0; i < max_tries; ++i) {
//     if (access(path.c_str(), F_OK) == 0) {
//       // Give udev a short moment to settle
//       std::this_thread::sleep_for(std::chrono::milliseconds(250));
//       return true;
//     }
//     std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
//   }
//   return false;
// }
// // -------------------- Helper: check transport (USB) using lsblk --------------------
// static bool IsUsbDevice(const std::string& cleaned_basename) {
//   // cleaned_basename should be something like "sda" or "nvme0n1"
//   std::string cmd = "lsblk -no TRAN /dev/" + cleaned_basename;
//   FILE* p = popen(cmd.c_str(), "r");
//   if (!p) return false;

//   char buf[256];
//   bool is_usb = false;
//   if (fgets(buf, sizeof(buf), p)) {
//     std::string s(buf);
//     // trim newline/spaces
//     while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || isspace((unsigned char)s.back())))
//       s.pop_back();
//     if (!s.empty() && s == "usb")
//       is_usb = true;
//   }
//   pclose(p);
//   return is_usb;
// }


// // -------------------- Disk helpers --------------------
// static uint64_t GetDiskSize(const std::string& device_path) {
//   Log("Attempting to get size for device: %s", device_path.c_str());

//   int fd = open(device_path.c_str(), O_RDONLY);
//   if (fd < 0) {
//     Log("Failed to open device %s: %s (errno: %d)", device_path.c_str(), strerror(errno), errno);
//     // fallback to sysfs sectors
//     std::string device_name = device_path.substr(device_path.find_last_of('/') + 1);
//     std::string sysfs_path = "/sys/class/block/" + device_name + "/size";
//     Log("Trying sysfs method: %s", sysfs_path.c_str());
//     std::ifstream size_file(sysfs_path);
//     if (size_file.is_open()) {
//       uint64_t sectors = 0;
//       size_file >> sectors;
//       size_file.close();
//       uint64_t size_bytes = sectors * 512ULL;
//       Log("Got size from sysfs: %lu bytes", size_bytes);
//       return size_bytes;
//     } else {
//       Log("Sysfs method also failed");
//     }
//     return 0;
//   }

//   uint64_t size = 0;
//   if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
//     close(fd);
//     Log("BLKGETSIZE64 succeeded: %lu bytes", size);
//     return size;
//   } else {
//     Log("BLKGETSIZE64 failed: %s (errno: %d)", strerror(errno), errno);
//   }

//   unsigned long sectors = 0;
//   if (ioctl(fd, BLKGETSIZE, &sectors) == 0) {
//     close(fd);
//     size = (uint64_t)sectors * 512ULL;
//     Log("BLKGETSIZE succeeded: %lu sectors = %lu bytes", sectors, size);
//     return size;
//   } else {
//     Log("BLKGETSIZE failed: %s (errno: %d)", strerror(errno), errno);
//   }

//   struct stat st;
//   if (fstat(fd, &st) == 0) {
//     if (S_ISREG(st.st_mode)) {
//       size = st.st_size;
//       Log("Regular file size: %lu bytes", size);
//     } else if (S_ISBLK(st.st_mode)) {
//       Log("Block device detected, trying lseek...");
//       off_t end = lseek(fd, 0, SEEK_END);
//       if (end != -1) {
//         size = (uint64_t)end;
//         lseek(fd, 0, SEEK_SET);
//         Log("lseek size: %lu bytes", size);
//       } else {
//         Log("lseek failed: %s (errno: %d)", strerror(errno), errno);
//       }
//     }
//   } else {
//     Log("fstat failed: %s (errno: %d)", strerror(errno), errno);
//   }

//   close(fd);
//   Log("Final size result: %lu bytes", size);
//   return size;
// }

// // -------------------- Device name cleaning & disk list (fixed) --------------------

// // clean_dev: normalize device names into the block-device basename used in /sys/block
// // Examples:
// //   "/dev/sda1"      -> "sda"
// //   "sda1"           -> "sda"
// //   "/dev/nvme0n1p1" -> "nvme0n1"
// //   "nvme0n1p1"      -> "nvme0n1"
// static std::string clean_dev(std::string dev) {
//   if (dev.rfind("/dev/", 0) == 0)
//     dev = dev.substr(5);

//   // trim trailing newline/spaces
//   while (!dev.empty() && (dev.back() == '\n' || dev.back() == '\r' || dev.back() == ' ' || dev.back() == '\t'))
//     dev.pop_back();

//   // nvme devices use 'p' before partition number: nvme0n1p1 -> nvme0n1
//   if (dev.rfind("nvme", 0) == 0) {
//     size_t ppos = dev.find('p');
//     if (ppos != std::string::npos) {
//       dev = dev.substr(0, ppos);
//     }
//     return dev;
//   }

//   // mmcblk may be mmcblk0p1 -> treat like nvme (remove trailing 'p' + digits)
//   if (dev.rfind("mmcblk", 0) == 0) {
//     size_t ppos = dev.find('p');
//     if (ppos != std::string::npos) {
//       dev = dev.substr(0, ppos);
//     }
//     return dev;
//   }

//   // for typical sdX style names: remove trailing digits (sda1 -> sda)
//   while (!dev.empty() && isdigit(static_cast<unsigned char>(dev.back())))
//     dev.pop_back();

//   // remove trailing 'p' if left behind
//   if (!dev.empty() && dev.back() == 'p')
//     dev.pop_back();

//   return dev;
// }

// // GetDiskInfo: returns only disks (type == "disk") and excludes the boot disk.
// // This implementation uses lsblk -J and compares the cleaned device names.
// static std::vector<std::string> GetDiskInfo() {
//   std::vector<std::string> disks;

//   // 1. Identify OS boot disk (the device that holds '/')
//   std::string root_dev;
//   {
//     FILE* p = popen("df / | tail -1 | awk '{print $1}'", "r");
//     if (p) {
//       char buf[128];
//       if (fgets(buf, sizeof(buf), p)) {
//         root_dev = buf;
//         // strip newline
//         while (!root_dev.empty() && (root_dev.back() == '\n' || root_dev.back() == '\r'))
//           root_dev.pop_back();
//       }
//       pclose(p);
//     }
//   }
//   std::string boot_disk = clean_dev(root_dev);
//   Log("Boot device detected as: '%s' (cleaned: '%s')", root_dev.c_str(), boot_disk.c_str());

//   // 2. Query lsblk in JSON
//   FILE* pipe = popen("lsblk -J -o NAME,SIZE,MODEL,TYPE", "r");
//   if (!pipe) {
//     disks.push_back("Error: Cannot get disk info");
//     return disks;
//   }

//   char buffer[8192];
//   std::string json;
//   while (fgets(buffer, sizeof(buffer), pipe))
//     json += buffer;
//   pclose(pipe);

//   size_t pos = 0;
//   while ((pos = json.find("\"name\":", pos)) != std::string::npos) {
//     auto extract = [&](const std::string& key) {
//       size_t k = json.find(key, pos);
//       if (k == std::string::npos) return std::string("");
//       size_t q1 = json.find("\"", k + key.size());
//       if (q1 == std::string::npos) return std::string("");
//       size_t q2 = json.find("\"", q1 + 1);
//       if (q2 == std::string::npos) return std::string("");
//       return json.substr(q1 + 1, q2 - q1 - 1);
//     };

//     std::string name  = extract("\"name\":");
//     std::string size  = extract("\"size\":");
//     std::string model = extract("\"model\":");
//     std::string type  = extract("\"type\":");

//     if (type == "disk") {
//       std::string cleaned_name = clean_dev(name);

//       // SKIP OS DISK (compare cleaned names)
//       if (!boot_disk.empty() && cleaned_name == boot_disk) {
//         pos += 6;
//         continue;
//       }

//       if (model.empty()) model = "Unknown";
//       if (size.empty()) size = "Unknown";

//       disks.push_back("NAME=" + name + " SIZE=" + size + " MODEL=" + model);
//     }

//     pos += 6;
//   }

//   if (disks.empty())
//     disks.push_back("No external disks found");

//   return disks;
// }

// // -------------------- Flutter progress updates --------------------
// static void SendProgressUpdate(int pass, int progress, const std::string& status) {
//   if (!g_method_channel) return;

//   g_main_context_invoke(nullptr, [](gpointer data) -> gboolean {
//     auto* msg = static_cast<std::tuple<int,int,std::string>*>(data);
//     int pass = std::get<0>(*msg);
//     int progress = std::get<1>(*msg);
//     std::string status = std::get<2>(*msg);
//     delete msg;

//     g_autoptr(FlValue) result = fl_value_new_map();
//     fl_value_set_string_take(result, "pass", fl_value_new_int(pass));
//     fl_value_set_string_take(result, "progress", fl_value_new_int(progress));
//     fl_value_set_string_take(result, "status", fl_value_new_string(status.c_str()));

//     fl_method_channel_invoke_method(
//       g_method_channel,
//       "onWipeProgress",
//       result,
//       nullptr,
//       nullptr,
//       nullptr
//     );

//     return FALSE;
//   }, new std::tuple<int,int,std::string>(pass, progress, status));
// }

// // -------------------- Overwrite routines --------------------
// static bool OverwriteWithZeros(int fd, uint64_t size, int passNumber=1, const std::string& label="Writing zeros...") {
//   const size_t buffer_size = 1024 * 1024;
//   std::vector<char> buffer(buffer_size, 0x00);
//   lseek(fd, 0, SEEK_SET);
//   uint64_t bytes_written = 0;
//   while (bytes_written < size && g_wiping_active) {
//     size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
//     ssize_t written = write(fd, buffer.data(), to_write);
//     if (written <= 0) return false;
//     bytes_written += (uint64_t)written;
//     if ((bytes_written % (buffer_size * 10)) == 0) {
//       int progress = (int)((bytes_written * 100) / size);
//       SendProgressUpdate(passNumber, progress, label);
//     }
//     if ((bytes_written % (buffer_size * 100)) == 0) fsync(fd);
//     std::this_thread::sleep_for(std::chrono::milliseconds(1));
//   }
//   fsync(fd);
//   return g_wiping_active;
// }

// static bool OverwriteWithOnes(int fd, uint64_t size) {
//   const size_t buffer_size = 1024 * 1024;
//   std::vector<char> buffer(buffer_size, (char)0xFF);
//   lseek(fd, 0, SEEK_SET);
//   uint64_t bytes_written = 0;
//   while (bytes_written < size && g_wiping_active) {
//     size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
//     ssize_t written = write(fd, buffer.data(), to_write);
//     if (written <= 0) return false;
//     bytes_written += (uint64_t)written;
//     if ((bytes_written % (buffer_size * 10)) == 0) {
//       int progress = (int)((bytes_written * 100) / size);
//       SendProgressUpdate(2, progress, "Pass 2: Writing ones...");
//     }
//     if ((bytes_written % (buffer_size * 100)) == 0) fsync(fd);
//     std::this_thread::sleep_for(std::chrono::milliseconds(1));
//   }
//   fsync(fd);
//   return g_wiping_active;
// }

// static bool OverwriteWithRandom(int fd, uint64_t size, int passNumber=3, const std::string& label="Writing random data...") {
//   const size_t buffer_size = 1024 * 1024;
//   std::vector<char> buffer(buffer_size);
//   std::random_device rd;
//   std::mt19937 gen(rd());
//   std::uniform_int_distribution<> dis(0, 255);
//   lseek(fd, 0, SEEK_SET);
//   uint64_t bytes_written = 0;
//   while (bytes_written < size && g_wiping_active) {
//     size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
//     for (size_t i = 0; i < to_write; ++i) buffer[i] = static_cast<char>(dis(gen));
//     ssize_t written = write(fd, buffer.data(), to_write);
//     if (written <= 0) return false;
//     bytes_written += (uint64_t)written;
//     if ((bytes_written % (buffer_size * 10)) == 0) {
//       int progress = (int)((bytes_written * 100) / size);
//       SendProgressUpdate(passNumber, progress, label);
//     }
//     if ((bytes_written % (buffer_size * 100)) == 0) fsync(fd);
//     std::this_thread::sleep_for(std::chrono::milliseconds(1));
//   }
//   fsync(fd);
//   return g_wiping_active;
// }

// // -------------------- Gutmann helpers --------------------
// static bool OverwritePattern(int fd, uint64_t size, unsigned char pattern, int passNumber, const std::string& label) {
//   const size_t buffer_size = 1024 * 1024;
//   std::vector<char> buffer(buffer_size, (char)pattern);
//   lseek(fd, 0, SEEK_SET);
//   uint64_t bytes_written = 0;
//   while (bytes_written < size && g_wiping_active) {
//     size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
//     ssize_t written = write(fd, buffer.data(), to_write);
//     if (written <= 0) return false;
//     bytes_written += (uint64_t)written;
//     if ((bytes_written % (buffer_size * 10)) == 0) {
//       int progress = (int)((bytes_written * 100) / size);
//       SendProgressUpdate(passNumber, progress, label);
//     }
//   }
//   fsync(fd);
//   return g_wiping_active;
// }

// static bool OverwriteRandomGutmann(int fd, uint64_t size, int passNumber) {
//   const size_t buffer_size = 1024 * 1024;
//   std::vector<char> buffer(buffer_size);
//   std::random_device rd;
//   std::mt19937 gen(rd());
//   std::uniform_int_distribution<> dis(0, 255);
//   lseek(fd, 0, SEEK_SET);
//   uint64_t bytes_written = 0;
//   while (bytes_written < size && g_wiping_active) {
//     size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
//     for (size_t i = 0; i < to_write; ++i) buffer[i] = static_cast<char>(dis(gen));
//     ssize_t written = write(fd, buffer.data(), to_write);
//     if (written <= 0) return false;
//     bytes_written += (uint64_t)written;
//     if ((bytes_written % (buffer_size * 10)) == 0) {
//       int progress = (int)((bytes_written * 100) / size);
//       SendProgressUpdate(passNumber, progress, "Gutmann Random Pass");
//     }
//   }
//   fsync(fd);
//   return g_wiping_active;
// }

// // -------------------- Unmount + kill helpers --------------------
// static void UnmountAndKill(const std::string& device_path) {
//   std::string dev = device_path;
//   std::string partnode;

//   if (dev.rfind("/dev/", 0) != 0) dev = "/dev/" + dev;

//   if (std::isdigit(dev.back())) {
//     partnode = dev;
//     std::string base = dev.substr(5);
//     while (!base.empty() && std::isdigit(base.back())) base.pop_back();
//     if (!base.empty() && base.back() == 'p') base.pop_back();
//     dev = "/dev/" + base;
//   } else {
//     if (dev.find("nvme") != std::string::npos)
//       partnode = dev + "p1";
//     else
//       partnode = dev + "1";
//   }

//   RunCmdQuiet("sudo umount " + partnode);
//   RunCmdQuiet("sudo umount " + dev);

//   RunCmdQuiet("sudo fuser -km " + dev);
//   RunCmdQuiet("sudo fuser -km " + partnode);

//   RunCmdQuiet("sudo umount -l " + partnode);
//   RunCmdQuiet("sudo umount -l " + dev);

//   RunCmdQuiet("sync");
//   std::this_thread::sleep_for(std::chrono::milliseconds(300));
// }

// // -------------------- Filesystem creation (robust) --------------------
// static bool CreateNewFileSystem(const std::string& device_path) {
//   SendProgressUpdate(5, 0, "Preparing to create new MBR partition table...");
//   UnmountAndKill(device_path);

//   std::string parted_label = "sudo parted -s " + device_path + " mklabel msdos";
//   if (!RunCmdQuiet(parted_label)) {
//     SendProgressUpdate(5, 0, "ERROR: Failed to create MBR partition table.");
//     return false;
//   }
//   SendProgressUpdate(5, 25, "MBR partition table created.");

//   SendProgressUpdate(5, 30, "Creating primary partition...");
//   std::string parted_part = "sudo parted -s " + device_path + " mkpart primary 1MiB 100%";
//   if (!RunCmdQuiet(parted_part)) {
//     SendProgressUpdate(5, 0, "ERROR: Failed to create primary partition.");
//     return false;
//   }

//   RunCmdQuiet("sudo parted -s " + device_path + " set 1 msftdata on");
//   SendProgressUpdate(5, 50, "Primary NTFS-compatible partition created.");

//   std::string expected_partition;
//   if (device_path.find("nvme") != std::string::npos) {
//     expected_partition = device_path + "p1";
//   } else {
//     expected_partition = device_path + "1";
//   }

//   SendProgressUpdate(5, 55, "Informing kernel about partition changes...");
//   for (int i = 0; i < 6; ++i) {
//     RunCmdQuiet("sudo partprobe " + device_path);
//     RunCmdQuiet("sudo udevadm trigger");
//     RunCmdQuiet("sudo udevadm settle");
//     if (access(expected_partition.c_str(), F_OK) == 0) break;
//     std::this_thread::sleep_for(std::chrono::milliseconds(400));
//   }

//   // Use WaitForDeviceStable to handle flaky devices
//   bool partition_exists = WaitForDeviceStable(expected_partition, 20, 300);
//   if (!partition_exists) {
//     SendProgressUpdate(5, 58, "WARNING: Partition node not visible; will attempt formatting fallback.");
//   }

//   std::string target = partition_exists ? expected_partition : device_path;

//   // Ensure device/partition is stable before formatting
//   if (!WaitForDeviceStable(target, 30, 300)) {
//     SendProgressUpdate(5, 0, "ERROR: Device not stable for formatting.");
//     return false;
//   }

//   SendProgressUpdate(5, 60, std::string("Formatting as NTFS (Label: Code_wipe) on ") + target + " ...");
//   std::string mkfs = "sudo mkfs.ntfs -F -f -L \"Code_wipe\" " + target;
//   if (!RunCmdQuiet(mkfs)) {
//     SendProgressUpdate(5, 0, "ERROR: NTFS format failed.");
//     return false;
//   }

//   RunCmdQuiet("sudo partprobe " + device_path);
//   RunCmdQuiet("sync");
//   std::this_thread::sleep_for(std::chrono::milliseconds(500));
//   SendProgressUpdate(5, 100, "NTFS filesystem created successfully!");
//   return true;
// }

// // -------------------- Wipe flows --------------------
// // (unchanged wipe flows follow - omitted for brevity in this comment, they are present in file)
// static void PerformDoD522022MWipe(const std::string& device_path) {
//   g_wiping_active = true;
//   auto start_time = std::chrono::steady_clock::now();
//   auto start_epoch = std::time(nullptr);
//   Log("==========================================");
//   Log("Wipe Started at (epoch): %lld", (long long)start_epoch);
//   Log("==========================================");

//   char dbg[256];
//   snprintf(dbg, sizeof(dbg), "Received device path: '%s'", device_path.c_str());
//   SendProgressUpdate(0, 0, dbg);
//   SendProgressUpdate(0, 0, "Initializing DoD 5220.22-M wipe...");

//   uint64_t device_size = GetDiskSize(device_path);
//   if (device_size == 0) {
//     SendProgressUpdate(0, 0, "Error: Cannot determine device size.");
//     g_wiping_active = false;
//     return;
//   }

//   char size_str[128];
//   snprintf(size_str, sizeof(size_str), "Device size: %.2f GB", device_size / (1024.0 * 1024.0 * 1024.0));
//   SendProgressUpdate(0, 0, size_str);

//   int fd = open(device_path.c_str(), O_WRONLY);
//   if (fd < 0) {
//     char em[256];
//     snprintf(em, sizeof(em), "Error: Cannot open device for writing: %s (errno: %d)", strerror(errno), errno);
//     SendProgressUpdate(0, 0, em);
//     g_wiping_active = false;
//     return;
//   }

//   SendProgressUpdate(1, 0, "Starting Pass 1: Writing zeros...");
//   if (!OverwriteWithZeros(fd, device_size, 1, "DoD Pass 1: Writing zeros...")) {
//     SendProgressUpdate(1, 0, g_wiping_active ? "Error in Pass 1" : "Wiping cancelled");
//     close(fd);
//     g_wiping_active = false;
//     return;
//   }
//   SendProgressUpdate(1, 100, "Pass 1 completed: All zeros written");

//   SendProgressUpdate(2, 0, "Starting Pass 2: Writing ones...");
//   if (!OverwriteWithOnes(fd, device_size)) {
//     SendProgressUpdate(2, 0, g_wiping_active ? "Error in Pass 2" : "Wiping cancelled");
//     close(fd);
//     g_wiping_active = false;
//     return;
//   }
//   SendProgressUpdate(2, 100, "Pass 2 completed: All ones written");

//   SendProgressUpdate(3, 0, "Starting Pass 3: Writing random data...");
//   if (!OverwriteWithRandom(fd, device_size, 3, "DoD Pass 3: Writing random data...")) {
//     SendProgressUpdate(3, 0, g_wiping_active ? "Error in Pass 3" : "Wiping cancelled");
//     close(fd);
//     g_wiping_active = false;
//     return;
//   }
//   SendProgressUpdate(3, 100, "Pass 3 completed: Random data written");

//   fsync(fd);
//   close(fd);

//   if (g_wiping_active)
//     SendProgressUpdate(4, 100, "DoD 5220.22-M wipe completed successfully!");

//   g_wiping_active = false;
//   SendProgressUpdate(4, 0, "Recreating filesystem...");
//   CreateNewFileSystem(device_path);
//   SendProgressUpdate(6, 100, "DoD wipe + NTFS rebuild complete.");

//   auto end_time = std::chrono::steady_clock::now();
//   auto end_epoch = std::time(nullptr);
//   auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

//   Log("==========================================");
//   Log("Wipe Finished at (epoch): %lld", (long long)end_epoch);
//   Log("Total Time Taken: %lld seconds", (long long)elapsed);
//   Log("==========================================");
// }

// // NIST Clear
// static void PerformNISTClear(const std::string& device_path) {
//   g_wiping_active = true;
//   auto start_time = std::chrono::steady_clock::now();
//   auto start_epoch = std::time(nullptr);
//   Log("==========================================");
//   Log("Wipe Started at (epoch): %lld", (long long)start_epoch);
//   Log("==========================================");

//   SendProgressUpdate(0, 0, "Starting NIST SP 800-88 CLEAR...");

//   uint64_t device_size = GetDiskSize(device_path);
//   if (device_size == 0) {
//     SendProgressUpdate(0, 0, "ERROR: Unable to read device size.");
//     g_wiping_active = false;
//     return;
//   }

//   int fd = open(device_path.c_str(), O_WRONLY);
//   if (fd < 0) {
//     SendProgressUpdate(0, 0, std::string("ERROR opening device: ") + strerror(errno));
//     g_wiping_active = false;
//     return;
//   }

//   SendProgressUpdate(1, 0, "NIST Clear Pass: Writing zeros...");
//   if (!OverwriteWithZeros(fd, device_size, 1, "NIST: Zero-fill pass...")) {
//     SendProgressUpdate(1, 0, "ERROR: Zero-fill failed.");
//     close(fd);
//     g_wiping_active = false;
//     return;
//   }
//   SendProgressUpdate(1, 100, "Zero-fill complete.");
//   fsync(fd);
//   close(fd);

//   SendProgressUpdate(4, 30, "Wiping partition metadata...");
//   fd = open(device_path.c_str(), O_WRONLY);
//   if (fd >= 0) {
//     const size_t md = 1024 * 1024;
//     std::vector<char> z(md, 0);

//     lseek(fd, 0, SEEK_SET);
//     write(fd, z.data(), md);
//     fsync(fd);

//     if (device_size > md) {
//       lseek(fd, device_size - md, SEEK_SET);
//       write(fd, z.data(), md);
//       fsync(fd);
//     }

//     close(fd);
//   }
//   SendProgressUpdate(4, 60, "Metadata wiped.");

//   SendProgressUpdate(4, 80, "Reloading partition table...");
//   RunCmdQuiet(std::string("sudo partprobe ") + device_path);

//   SendProgressUpdate(5, 0, "Recreating filesystem...");
//   if (CreateNewFileSystem(device_path)) {
//     SendProgressUpdate(6, 100, "NIST CLEAR completed successfully!");
//   } else {
//     SendProgressUpdate(6, 0, "NIST CLEAR complete, but filesystem creation failed.");
//   }

//   g_wiping_active = false;
//   auto end_time = std::chrono::steady_clock::now();
//   auto end_epoch = std::time(nullptr);
//   auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

//   Log("==========================================");
//   Log("Wipe Finished at (epoch): %lld", (long long)end_epoch);
//   Log("Total Time Taken: %lld seconds", (long long)elapsed);
//   Log("==========================================");
// }

// // Gutmann (simplified 4-pass)
// static void PerformGutmannWipe(const std::string& device_path) {
//   g_wiping_active = true;
//   auto start_time = std::chrono::steady_clock::now();
//   auto start_epoch = std::time(nullptr);
//   Log("==========================================");
//   Log("Wipe Started at (epoch): %lld", (long long)start_epoch);
//   Log("==========================================");

//   SendProgressUpdate(10, 0, "Starting Gutmann Method...");

//   uint64_t size = GetDiskSize(device_path);
//   if (size == 0) {
//     SendProgressUpdate(10, 0, "ERROR: Unable to read disk size");
//     g_wiping_active = false;
//     return;
//   }

//   int fd = open(device_path.c_str(), O_WRONLY);
//   if (fd < 0) {
//     SendProgressUpdate(10, 0, "ERROR opening device");
//     g_wiping_active = false;
//     return;
//   }

//   SendProgressUpdate(10, 0, "Gutmann Pass 1: Random");
//   if (!OverwriteRandomGutmann(fd, size, 10)) { close(fd); g_wiping_active = false; return; }
//   SendProgressUpdate(10, 100, "Gutmann Pass 1 complete");

//   SendProgressUpdate(11, 0, "Gutmann Pass 2: 0x55");
//   if (!OverwritePattern(fd, size, 0x55, 11, "Gutmann Pass 2: 0x55")) { close(fd); g_wiping_active = false; return; }
//   SendProgressUpdate(11, 100, "Gutmann Pass 2 complete");

//   SendProgressUpdate(12, 0, "Gutmann Pass 3: 0xAA");
//   if (!OverwritePattern(fd, size, 0xAA, 12, "Gutmann Pass 3: 0xAA")) { close(fd); g_wiping_active = false; return; }
//   SendProgressUpdate(12, 100, "Gutmann Pass 3 complete");

//   SendProgressUpdate(13, 0, "Gutmann Pass 4: Random");
//   if (!OverwriteRandomGutmann(fd, size, 13)) { close(fd); g_wiping_active = false; return; }
//   SendProgressUpdate(13, 100, "Gutmann Pass 4 complete");

//   close(fd);

//   SendProgressUpdate(13, 0, "Creating NTFS filesystem...");
//   CreateNewFileSystem(device_path);
//   SendProgressUpdate(13, 100, "Gutmann wipe complete.");
//   g_wiping_active = false;
//   auto end_time = std::chrono::steady_clock::now();
//   auto end_epoch = std::time(nullptr);
//   auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

//   Log("==========================================");
//   Log("Wipe Finished at (epoch): %lld", (long long)end_epoch);
//   Log("Total Time Taken: %lld seconds", (long long)elapsed);
//   Log("==========================================");
// }

// // Single Pass Zero
// static void PerformSinglePassZero(const std::string& device_path) {
//   g_wiping_active = true;
//   auto start_time = std::chrono::steady_clock::now();
//   auto start_epoch = std::time(nullptr);
//   Log("==========================================");
//   Log("Wipe Started at (epoch): %lld", (long long)start_epoch);
//   Log("==========================================");

//   SendProgressUpdate(20, 0, "Starting Single Pass Zero...");

//   uint64_t size = GetDiskSize(device_path);
//   if (size == 0) {
//     SendProgressUpdate(20, 0, "ERROR: Unable to read disk size");
//     g_wiping_active = false;
//     return;
//   }

//   int fd = open(device_path.c_str(), O_WRONLY);
//   if (fd < 0) {
//     SendProgressUpdate(20, 0, "ERROR opening device");
//     g_wiping_active = false;
//     return;
//   }

//   SendProgressUpdate(20, 0, "Single Pass: Writing zeros...");
//   if (!OverwriteWithZeros(fd, size, 20, "Single Pass: Writing zeros...")) { close(fd); g_wiping_active = false; return; }
//   SendProgressUpdate(20, 100, "Zero-fill complete");
//   close(fd);

//   SendProgressUpdate(21, 20, "Wiping metadata...");
//   RunCmdQuiet("sudo dd if=/dev/zero of=" + device_path + " bs=1M count=1");
//   SendProgressUpdate(21, 100, "Metadata wiped");

//   SendProgressUpdate(22, 0, "Creating filesystem...");
//   CreateNewFileSystem(device_path);
//   SendProgressUpdate(22, 100, "Single pass zero complete.");
//   g_wiping_active = false;
//   auto end_time = std::chrono::steady_clock::now();
//   auto end_epoch = std::time(nullptr);
//   auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

//   Log("==========================================");
//   Log("Wipe Finished at (epoch): %lld", (long long)end_epoch);
//   Log("Total Time Taken: %lld seconds", (long long)elapsed);
//   Log("==========================================");
// }

// // -------------------- Combined entrypoint with safety --------------------
// static void CompletelyWipeDisk(const std::string& device_path, const std::string& mode) {

//   // ------------------ CLEAN INPUT DEVICE NAME ------------------
//   std::string cleaned = clean_dev(device_path);

//   // ------------------ FIND ACTUAL SYSTEM BOOT DISK ------------------
//   std::string boot_disk;
//   {
//     FILE* p = popen("df / | tail -1 | awk '{print $1}'", "r");
//     if (p) {
//       char buf[128];
//       if (fgets(buf, sizeof(buf), p)) {
//         boot_disk = clean_dev(buf);
//       }
//       pclose(p);
//     }
//   }

//   // ------------------ BLOCK SYSTEM DISK ALWAYS ------------------
//   if (cleaned == boot_disk) {
//     SendProgressUpdate(0, 0, "❌ Refusing to wipe SYSTEM DISK!");
//     return;
//   }

//   // ------------------ READ REMOVABLE FLAG ------------------
//   std::string sys_rem = "/sys/block/" + cleaned + "/removable";
//   std::ifstream f(sys_rem);
//   int removable = 0;

//   if (f.is_open()) {
//     f >> removable;
//     f.close();
//   } else {
//     SendProgressUpdate(0, 0, "WARNING: Could not read removable flag. Allowing ONLY if NOT system disk.");
//     removable = 1;  // fallback: treat unknown devices as removable only if they are not boot-disk
//   }

//   // ------------------ BLOCK NON-REMOVABLE Nvme/SATA EXCEPT EXTERNAL HDD ------------------
//   if (removable == 0) {
//     SendProgressUpdate(0, 0, "Device marked non-removable (RM=0). Checking if safe...");

//     // Allow external USB HDDs that incorrectly report RM=0
//     // std::string bus_path = "/sys/block/" + cleaned + "/device";
//     // bool is_usb = false;

//     // for (int i = 0; i < 3; i++) {
//     //   std::string chk = bus_path + "/../..";
//     //   bus_path = chk;
//     //   char linkbuf[512];
//     //   ssize_t len = readlink(chk.c_str(), linkbuf, sizeof(linkbuf) - 1);
//     //   if (len > 0) {
//     //     linkbuf[len] = 0;
//     //     if (std::string(linkbuf).find("usb") != std::string::npos) {
//     //       is_usb = true;
//     //       break;
//     //     }
//     //   }
//     // }

//     // if (!is_usb) {
//     //   SendProgressUpdate(0, 0, "❌ Refusing to wipe INTERNAL DISK (non-removable, non-USB).");
//     //   return;
//     // }

//     // SendProgressUpdate(0, 0, "⚠ RM=0 but device is USB → Allowing external HDD wipe.");
//         // Allow external USB HDDs that incorrectly report RM=0 by checking transport
//     bool is_usb = IsUsbDevice(cleaned);

//     if (!is_usb) {
//       SendProgressUpdate(0, 0, "❌ Refusing to wipe INTERNAL DISK (non-removable, non-USB).");
//       return;
//     }

//     SendProgressUpdate(0, 0, "⚠ RM=0 but device TRAN=usb → Allowing external HDD wipe.");

//   }

//   // ------------------ SELECT WIPE MODE ------------------
//   if (mode == "nist")
//     PerformNISTClear(device_path);
//   else if (mode == "gutmann")
//     PerformGutmannWipe(device_path);
//   else if (mode == "singlepass")
//     PerformSinglePassZero(device_path);
//   else
//     PerformDoD522022MWipe(device_path);
// }

// // -------------------- Method channel handler --------------------
// static void method_call_handler(FlMethodChannel* channel, FlMethodCall* method_call, gpointer user_data) {
//   const gchar* method = fl_method_call_get_name(method_call);

//   if (g_strcmp0(method, "getDiskInfo") == 0) {
//     std::vector<std::string> disk_info = GetDiskInfo();
//     g_autoptr(FlValue) result = fl_value_new_list();
//     for (const auto& d : disk_info) fl_value_append_take(result, fl_value_new_string(d.c_str()));
//     fl_method_call_respond_success(method_call, result, nullptr);

//   } else if (g_strcmp0(method, "startDoD522022MWipe") == 0) {
//     FlValue* args = fl_method_call_get_args(method_call);
//     FlValue* dv = args ? fl_value_lookup_string(args, "devicePath") : nullptr;
//     const char* device_path_c = dv ? fl_value_get_string(dv) : nullptr;
//     if (!device_path_c) {
//       fl_method_call_respond_error(method_call, "INVALID_ARGS", "Missing devicePath", nullptr, nullptr);
//       return;
//     }
//     std::string device_path(device_path_c);

//     if (g_wiping_active) {
//       fl_method_call_respond_error(method_call, "WIPE_IN_PROGRESS", "Another wipe operation is already in progress", nullptr, nullptr);
//       return;
//     }
//     std::thread t([device_path]() { PerformDoD522022MWipe(device_path); });
//     t.detach();
//     fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);

//   } else if (g_strcmp0(method, "completelyWipeDisk") == 0) {
//     FlValue* args = fl_method_call_get_args(method_call);
//     FlValue* dv = args ? fl_value_lookup_string(args, "devicePath") : nullptr;
//     const char* device_path_c = dv ? fl_value_get_string(dv) : nullptr;
//     if (!device_path_c) {
//       fl_method_call_respond_error(method_call, "INVALID_ARGS", "Missing devicePath", nullptr, nullptr);
//       return;
//     }
//     std::string device_path(device_path_c);

//     // keep old safety check here for the RPC call too
//     std::string dev_basename = device_path;
//     if (dev_basename.rfind("/dev/", 0) == 0) dev_basename = dev_basename.substr(5);
//     while (!dev_basename.empty() && std::isdigit(dev_basename.back())) dev_basename.pop_back();
//     if (!dev_basename.empty() && dev_basename.back() == 'p') dev_basename.pop_back();

//     // std::string sys_rem = "/sys/block/" + dev_basename + "/removable";
//     // std::ifstream f2(sys_rem);
//     // int removable = 0;
//     // if (f2.is_open()) {
//     //   f2 >> removable;
//     //   f2.close();
//     // } else {
//     //   fl_method_call_respond_error(method_call, "SYSFS_READ_FAILED", "Unable to determine if device is removable", nullptr, nullptr);
//     //   return;
//     // }
//     // if (removable == 0) {
//     //   fl_method_call_respond_error(method_call, "SYSTEM_DISK_BLOCKED", "Refusing to wipe internal system disk", nullptr, nullptr);
//     //   return;
//     // }
//     std::string sys_rem = "/sys/block/" + dev_basename + "/removable";
//     std::ifstream f2(sys_rem);
//     int removable = 0;
//     if (f2.is_open()) {
//       f2 >> removable;
//       f2.close();
//     } else {
//       // If we cannot read removable flag, try to fall back to TRAN; but if TRAN
//       // can't be determined either, fail safely.
//       bool usb_fallback = IsUsbDevice(dev_basename);
//       if (!usb_fallback) {
//         fl_method_call_respond_error(method_call, "SYSFS_READ_FAILED", "Unable to determine if device is removable and transport is not USB", nullptr, nullptr);
//         return;
//       }
//       removable = 1; // treat as removable because it's a USB device
//     }

//     if (removable == 0) {
//       // if RM=0, allow if transport is USB (external HDD incorrectly reporting RM=0)
//       if (!IsUsbDevice(dev_basename)) {
//         fl_method_call_respond_error(method_call, "SYSTEM_DISK_BLOCKED", "Refusing to wipe internal system disk", nullptr, nullptr);
//         return;
//       }
//       // otherwise it's USB -> allow
//     }


//     FlValue* mode_val = args ? fl_value_lookup_string(args, "mode") : nullptr;
//     const char* mode_c = mode_val ? fl_value_get_string(mode_val) : nullptr;
//     std::string mode = mode_c ? mode_c : "dod";

//     std::thread t([device_path, mode]() { CompletelyWipeDisk(device_path, mode); });
//     t.detach();
//     fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);

//   } else if (g_strcmp0(method, "cancelWipe") == 0) {
//     g_wiping_active = false;
//     SendProgressUpdate(0, 0, "Wipe operation cancelled by user");
//     fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);
//   } else {
//     fl_method_call_respond_not_implemented(method_call, nullptr);
//   }
// }

// // -------------------- GApplication callbacks --------------------
// static void my_application_activate(GApplication* application) {
//   MyApplication* self = MY_APPLICATION(application);
//   GtkWindow* window = GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application)));

//   gboolean use_header_bar = TRUE;
// #ifdef GDK_WINDOWING_X11
//   GdkScreen* screen = gtk_window_get_screen(window);
//   if (GDK_IS_X11_SCREEN(screen)) {
//     const gchar* wm_name = gdk_x11_screen_get_window_manager_name(screen);
//     if (g_strcmp0(wm_name, "GNOME Shell") != 0) {
//       use_header_bar = FALSE;
//     }
//   }
// #endif
//   if (use_header_bar) {
//     GtkHeaderBar* header_bar = GTK_HEADER_BAR(gtk_header_bar_new());
//     gtk_widget_show(GTK_WIDGET(header_bar));
//     gtk_header_bar_set_title(header_bar, "CODEWIPE");
//     gtk_header_bar_set_show_close_button(header_bar, TRUE);
//     gtk_window_set_titlebar(window, GTK_WIDGET(header_bar));
//   } else {
//     gtk_window_set_title(window, "CODEWIPE");
//   }

//   gtk_window_set_default_size(window, 1280, 720);
//   gtk_widget_show(GTK_WIDGET(window));

//   g_autoptr(FlDartProject) project = fl_dart_project_new();
//   fl_dart_project_set_dart_entrypoint_arguments(project, self->dart_entrypoint_arguments);

//   FlView* view = fl_view_new(project);
//   gtk_widget_show(GTK_WIDGET(view));
//   gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

//   fl_register_plugins(FL_PLUGIN_REGISTRY(view));

//   FlEngine* engine = fl_view_get_engine(view);
//   g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
//   FlMethodChannel* method_channel = fl_method_channel_new(
//       fl_engine_get_binary_messenger(engine),
//       "com.yourdomain.deviceinfo",
//       FL_METHOD_CODEC(codec));

//   g_method_channel = method_channel;
//   self->method_channel = method_channel;

//   fl_method_channel_set_method_call_handler(method_channel, method_call_handler, self, nullptr);

//   gtk_widget_grab_focus(GTK_WIDGET(view));
// }

// static gboolean my_application_local_command_line(GApplication* application, gchar*** arguments, int* exit_status) {
//   MyApplication* self = MY_APPLICATION(application);
//   self->dart_entrypoint_arguments = g_strdupv(*arguments + 1);

//   g_autoptr(GError) error = nullptr;
//   if (!g_application_register(application, nullptr, &error)) {
//      g_warning("Failed to register: %s", error->message);
//      *exit_status = 1;
//      return TRUE;
//   }

//   g_application_activate(application);
//   *exit_status = 0;
//   return TRUE;
// }

// static void my_application_startup(GApplication* application) {
//   G_APPLICATION_CLASS(my_application_parent_class)->startup(application);
// }
// static void my_application_shutdown(GApplication* application) {
//   G_APPLICATION_CLASS(my_application_parent_class)->shutdown(application);
// }
// static void my_application_dispose(GObject* object) {
//   MyApplication* self = MY_APPLICATION(object);
//   g_clear_pointer(&self->dart_entrypoint_arguments, g_strfreev);
//   G_OBJECT_CLASS(my_application_parent_class)->dispose(object);
// }
// static void my_application_class_init(MyApplicationClass* klass) {
//   G_APPLICATION_CLASS(klass)->activate = my_application_activate;
//   G_APPLICATION_CLASS(klass)->local_command_line = my_application_local_command_line;
//   G_APPLICATION_CLASS(klass)->startup = my_application_startup;
//   G_APPLICATION_CLASS(klass)->shutdown = my_application_shutdown;
//   G_OBJECT_CLASS(klass)->dispose = my_application_dispose;
// }
// static void my_application_init(MyApplication* self) {}
// MyApplication* my_application_new() {
//   return MY_APPLICATION(g_object_new(my_application_get_type(),
//                                      "application-id", APPLICATION_ID,
//                                      "flags", G_APPLICATION_NON_UNIQUE,
//                                      nullptr));
// }
// linux/my_application.cc
#include "my_application.h"

#include <flutter_linux/flutter_linux.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "flutter/generated_plugin_registrant.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include <cstdarg>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <random>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>
#include <tuple>
#include <algorithm>
#include <functional>
#include <sys/wait.h>

#ifndef APPLICATION_ID
#define APPLICATION_ID "com.example.code_wipe"
#endif

struct _MyApplication {
  GtkApplication parent_instance;
  char** dart_entrypoint_arguments;
  FlMethodChannel* method_channel;
};

G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION)

// -------------------- Globals --------------------
static FlMethodChannel* g_method_channel = nullptr;
static bool g_wiping_active = false;

// -------------------- Logging helper --------------------
static void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  printf("\n");
  fflush(stdout);
  va_end(ap);
}

// -------------------- Shell helper: run command quietly --------------------
static bool RunCmdQuiet(const std::string& cmd) {
  std::string full = cmd + " >/dev/null 2>&1";
  int rc = system(full.c_str());
  if (rc == -1) return false;
  return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

// -------------------- Wait for device / partition stabilization --------------------
static bool WaitForDeviceStable(const std::string& path, int max_tries = 30, int wait_ms = 300) {
  for (int i = 0; i < max_tries; ++i) {
    if (access(path.c_str(), F_OK) == 0) {
      // Give udev a short moment to settle
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
  }
  return false;
}
// -------------------- Helper: check transport (USB) using lsblk --------------------
static bool IsUsbDevice(const std::string& cleaned_basename) {
  // cleaned_basename should be something like "sda" or "nvme0n1"
  std::string cmd = "lsblk -no TRAN /dev/" + cleaned_basename;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return false;

  char buf[256];
  bool is_usb = false;
  if (fgets(buf, sizeof(buf), p)) {
    std::string s(buf);
    // trim newline/spaces
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || isspace((unsigned char)s.back())))
      s.pop_back();
    if (!s.empty() && s == "usb")
      is_usb = true;
  }
  pclose(p);
  return is_usb;
}


// -------------------- Disk helpers --------------------
static uint64_t GetDiskSize(const std::string& device_path) {
  Log("Attempting to get size for device: %s", device_path.c_str());

  int fd = open(device_path.c_str(), O_RDONLY);
  if (fd < 0) {
    Log("Failed to open device %s: %s (errno: %d)", device_path.c_str(), strerror(errno), errno);
    // fallback to sysfs sectors
    std::string device_name = device_path.substr(device_path.find_last_of('/') + 1);
    std::string sysfs_path = "/sys/class/block/" + device_name + "/size";
    Log("Trying sysfs method: %s", sysfs_path.c_str());
    std::ifstream size_file(sysfs_path);
    if (size_file.is_open()) {
      uint64_t sectors = 0;
      size_file >> sectors;
      size_file.close();
      uint64_t size_bytes = sectors * 512ULL;
      Log("Got size from sysfs: %lu bytes", size_bytes);
      return size_bytes;
    } else {
      Log("Sysfs method also failed");
    }
    return 0;
  }

  uint64_t size = 0;
  if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
    close(fd);
    Log("BLKGETSIZE64 succeeded: %lu bytes", size);
    return size;
  } else {
    Log("BLKGETSIZE64 failed: %s (errno: %d)", strerror(errno), errno);
  }

  unsigned long sectors = 0;
  if (ioctl(fd, BLKGETSIZE, &sectors) == 0) {
    close(fd);
    size = (uint64_t)sectors * 512ULL;
    Log("BLKGETSIZE succeeded: %lu sectors = %lu bytes", sectors, size);
    return size;
  } else {
    Log("BLKGETSIZE failed: %s (errno: %d)", strerror(errno), errno);
  }

  struct stat st;
  if (fstat(fd, &st) == 0) {
    if (S_ISREG(st.st_mode)) {
      size = st.st_size;
      Log("Regular file size: %lu bytes", size);
    } else if (S_ISBLK(st.st_mode)) {
      Log("Block device detected, trying lseek...");
      off_t end = lseek(fd, 0, SEEK_END);
      if (end != -1) {
        size = (uint64_t)end;
        lseek(fd, 0, SEEK_SET);
        Log("lseek size: %lu bytes", size);
      } else {
        Log("lseek failed: %s (errno: %d)", strerror(errno), errno);
      }
    }
  } else {
    Log("fstat failed: %s (errno: %d)", strerror(errno), errno);
  }

  close(fd);
  Log("Final size result: %lu bytes", size);
  return size;
}

// -------------------- Device name cleaning & disk list (fixed) --------------------

// clean_dev: normalize device names into the block-device basename used in /sys/block
// Examples:
//   "/dev/sda1"      -> "sda"
//   "sda1"           -> "sda"
//   "/dev/nvme0n1p1" -> "nvme0n1"
//   "nvme0n1p1"      -> "nvme0n1"
static std::string clean_dev(std::string dev) {
  if (dev.rfind("/dev/", 0) == 0)
    dev = dev.substr(5);

  // trim trailing newline/spaces
  while (!dev.empty() && (dev.back() == '\n' || dev.back() == '\r' || dev.back() == ' ' || dev.back() == '\t'))
    dev.pop_back();

  // nvme devices use 'p' before partition number: nvme0n1p1 -> nvme0n1
  if (dev.rfind("nvme", 0) == 0) {
    size_t ppos = dev.find('p');
    if (ppos != std::string::npos) {
      dev = dev.substr(0, ppos);
    }
    return dev;
  }

  // mmcblk may be mmcblk0p1 -> treat like nvme (remove trailing 'p' + digits)
  if (dev.rfind("mmcblk", 0) == 0) {
    size_t ppos = dev.find('p');
    if (ppos != std::string::npos) {
      dev = dev.substr(0, ppos);
    }
    return dev;
  }

  // for typical sdX style names: remove trailing digits (sda1 -> sda)
  while (!dev.empty() && isdigit(static_cast<unsigned char>(dev.back())))
    dev.pop_back();

  // remove trailing 'p' if left behind
  if (!dev.empty() && dev.back() == 'p')
    dev.pop_back();

  return dev;
}

// GetDiskInfo: returns only disks (type == "disk") and excludes the boot disk.
// This implementation uses lsblk -J and compares the cleaned device names.
static std::vector<std::string> GetDiskInfo() {
  std::vector<std::string> disks;

  // 1. Identify OS boot disk (the device that holds '/')
  std::string root_dev;
  {
    FILE* p = popen("df / | tail -1 | awk '{print $1}'", "r");
    if (p) {
      char buf[128];
      if (fgets(buf, sizeof(buf), p)) {
        root_dev = buf;
        // strip newline
        while (!root_dev.empty() && (root_dev.back() == '\n' || root_dev.back() == '\r'))
          root_dev.pop_back();
      }
      pclose(p);
    }
  }
  std::string boot_disk = clean_dev(root_dev);
  Log("Boot device detected as: '%s' (cleaned: '%s')", root_dev.c_str(), boot_disk.c_str());

  // 2. Query lsblk in JSON
  FILE* pipe = popen("lsblk -J -o NAME,SIZE,MODEL,TYPE", "r");
  if (!pipe) {
    disks.push_back("Error: Cannot get disk info");
    return disks;
  }

  char buffer[8192];
  std::string json;
  while (fgets(buffer, sizeof(buffer), pipe))
    json += buffer;
  pclose(pipe);

  size_t pos = 0;
  while ((pos = json.find("\"name\":", pos)) != std::string::npos) {
    auto extract = [&](const std::string& key) {
      size_t k = json.find(key, pos);
      if (k == std::string::npos) return std::string("");
      size_t q1 = json.find("\"", k + key.size());
      if (q1 == std::string::npos) return std::string("");
      size_t q2 = json.find("\"", q1 + 1);
      if (q2 == std::string::npos) return std::string("");
      return json.substr(q1 + 1, q2 - q1 - 1);
    };

    std::string name  = extract("\"name\":");
    std::string size  = extract("\"size\":");
    std::string model = extract("\"model\":");
    std::string type  = extract("\"type\":");

    if (type == "disk") {
      std::string cleaned_name = clean_dev(name);

      // SKIP OS DISK (compare cleaned names)
      if (!boot_disk.empty() && cleaned_name == boot_disk) {
        pos += 6;
        continue;
      }

      if (model.empty()) model = "Unknown";
      if (size.empty()) size = "Unknown";

      disks.push_back("NAME=" + name + " SIZE=" + size + " MODEL=" + model);
    }

    pos += 6;
  }

  if (disks.empty())
    disks.push_back("No external disks found");

  return disks;
}

// -------------------- Flutter progress updates --------------------
static void SendProgressUpdate(int pass, int progress, const std::string& status) {
  if (!g_method_channel) return;

  g_main_context_invoke(nullptr, [](gpointer data) -> gboolean {
    auto* msg = static_cast<std::tuple<int,int,std::string>*>(data);
    int pass = std::get<0>(*msg);
    int progress = std::get<1>(*msg);
    std::string status = std::get<2>(*msg);
    delete msg;

    g_autoptr(FlValue) result = fl_value_new_map();
    fl_value_set_string_take(result, "pass", fl_value_new_int(pass));
    fl_value_set_string_take(result, "progress", fl_value_new_int(progress));
    fl_value_set_string_take(result, "status", fl_value_new_string(status.c_str()));

    fl_method_channel_invoke_method(
      g_method_channel,
      "onWipeProgress",
      result,
      nullptr,
      nullptr,
      nullptr
    );

    return FALSE;
  }, new std::tuple<int,int,std::string>(pass, progress, status));
}

// -------------------- Overwrite routines --------------------
static bool OverwriteWithZeros(int fd, uint64_t size, int passNumber=1, const std::string& label="Writing zeros...") {
  const size_t buffer_size = 1024 * 1024;
  std::vector<char> buffer(buffer_size, 0x00);
  lseek(fd, 0, SEEK_SET);
  uint64_t bytes_written = 0;
  while (bytes_written < size && g_wiping_active) {
    size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
    ssize_t written = write(fd, buffer.data(), to_write);
    if (written <= 0) return false;
    bytes_written += (uint64_t)written;
    if ((bytes_written % (buffer_size * 10)) == 0) {
      int progress = (int)((bytes_written * 100) / size);
      SendProgressUpdate(passNumber, progress, label);
    }
    if ((bytes_written % (buffer_size * 100)) == 0) fsync(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  fsync(fd);
  return g_wiping_active;
}

static bool OverwriteWithOnes(int fd, uint64_t size) {
  const size_t buffer_size = 1024 * 1024;
  std::vector<char> buffer(buffer_size, (char)0xFF);
  lseek(fd, 0, SEEK_SET);
  uint64_t bytes_written = 0;
  while (bytes_written < size && g_wiping_active) {
    size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
    ssize_t written = write(fd, buffer.data(), to_write);
    if (written <= 0) return false;
    bytes_written += (uint64_t)written;
    if ((bytes_written % (buffer_size * 10)) == 0) {
      int progress = (int)((bytes_written * 100) / size);
      SendProgressUpdate(2, progress, "Pass 2: Writing ones...");
    }
    if ((bytes_written % (buffer_size * 100)) == 0) fsync(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  fsync(fd);
  return g_wiping_active;
}

static bool OverwriteWithRandom(int fd, uint64_t size, int passNumber=3, const std::string& label="Writing random data...") {
  const size_t buffer_size = 1024 * 1024;
  std::vector<char> buffer(buffer_size);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  lseek(fd, 0, SEEK_SET);
  uint64_t bytes_written = 0;
  while (bytes_written < size && g_wiping_active) {
    size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
    for (size_t i = 0; i < to_write; ++i) buffer[i] = static_cast<char>(dis(gen));
    ssize_t written = write(fd, buffer.data(), to_write);
    if (written <= 0) return false;
    bytes_written += (uint64_t)written;
    if ((bytes_written % (buffer_size * 10)) == 0) {
      int progress = (int)((bytes_written * 100) / size);
      SendProgressUpdate(passNumber, progress, label);
    }
    if ((bytes_written % (buffer_size * 100)) == 0) fsync(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  fsync(fd);
  return g_wiping_active;
}

// -------------------- Gutmann helpers --------------------
static bool OverwritePattern(int fd, uint64_t size, unsigned char pattern, int passNumber, const std::string& label) {
  const size_t buffer_size = 1024 * 1024;
  std::vector<char> buffer(buffer_size, (char)pattern);
  lseek(fd, 0, SEEK_SET);
  uint64_t bytes_written = 0;
  while (bytes_written < size && g_wiping_active) {
    size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
    ssize_t written = write(fd, buffer.data(), to_write);
    if (written <= 0) return false;
    bytes_written += (uint64_t)written;
    if ((bytes_written % (buffer_size * 10)) == 0) {
      int progress = (int)((bytes_written * 100) / size);
      SendProgressUpdate(passNumber, progress, label);
    }
  }
  fsync(fd);
  return g_wiping_active;
}

static bool OverwriteRandomGutmann(int fd, uint64_t size, int passNumber) {
  const size_t buffer_size = 1024 * 1024;
  std::vector<char> buffer(buffer_size);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  lseek(fd, 0, SEEK_SET);
  uint64_t bytes_written = 0;
  while (bytes_written < size && g_wiping_active) {
    size_t to_write = std::min(buffer_size, (size_t)(size - bytes_written));
    for (size_t i = 0; i < to_write; ++i) buffer[i] = static_cast<char>(dis(gen));
    ssize_t written = write(fd, buffer.data(), to_write);
    if (written <= 0) return false;
    bytes_written += (uint64_t)written;
    if ((bytes_written % (buffer_size * 10)) == 0) {
      int progress = (int)((bytes_written * 100) / size);
      SendProgressUpdate(passNumber, progress, "Gutmann Random Pass");
    }
  }
  fsync(fd);
  return g_wiping_active;
}

// -------------------- Unmount + kill helpers --------------------
static void UnmountAndKill(const std::string& device_path) {
  std::string dev = device_path;
  std::string partnode;

  if (dev.rfind("/dev/", 0) != 0) dev = "/dev/" + dev;

  if (std::isdigit(dev.back())) {
    partnode = dev;
    std::string base = dev.substr(5);
    while (!base.empty() && std::isdigit(base.back())) base.pop_back();
    if (!base.empty() && base.back() == 'p') base.pop_back();
    dev = "/dev/" + base;
  } else {
    if (dev.find("nvme") != std::string::npos)
      partnode = dev + "p1";
    else
      partnode = dev + "1";
  }

  RunCmdQuiet("sudo umount " + partnode);
  RunCmdQuiet("sudo umount " + dev);

  RunCmdQuiet("sudo fuser -km " + dev);
  RunCmdQuiet("sudo fuser -km " + partnode);

  RunCmdQuiet("sudo umount -l " + partnode);
  RunCmdQuiet("sudo umount -l " + dev);

  RunCmdQuiet("sync");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

// -------------------- Filesystem creation (robust) --------------------
// Modified to select MBR/GPT + NTFS/FAT32 depending solely on device_type
static bool CreateNewFileSystem(const std::string& device_path, const std::string& device_type = "Unknown") {
  SendProgressUpdate(5, 0, "Preparing to create new partition table and filesystem...");
  UnmountAndKill(device_path);

  // Decide partition table type (msdos = MBR, gpt = GPT)
  std::string partition_label = "msdos"; // default MBR
  std::string filesystem = "ntfs";       // default FS

  if (device_type == "Memory Card") {
    partition_label = "msdos";
    filesystem = "fat32";
  } else if (device_type == "Pendrive") {
    partition_label = "msdos";
    filesystem = "ntfs";
  } else if (device_type == "External HDD") {
    // IMPORTANT: user requested ALWAYS GPT + NTFS for External HDD
    partition_label = "gpt";
    filesystem = "ntfs";
  } else {
    // Fallback: msdos + ntfs
    partition_label = "msdos";
    filesystem = "ntfs";
  }

  // Create partition table
  std::string mklabel_cmd = "sudo parted -s " + device_path + " mklabel " + partition_label;
  if (!RunCmdQuiet(mklabel_cmd)) {
    SendProgressUpdate(5, 0, "ERROR: Failed to create partition table.");
    return false;
  }
  SendProgressUpdate(5, 25, std::string("Partition table created: ") + partition_label);

  SendProgressUpdate(5, 30, "Creating primary partition...");
  // If GPT, parted's mkpart can set type; use generic mkpart for compatibility
  std::string parted_part = "sudo parted -s " + device_path + " mkpart primary 1MiB 100%";
  if (!RunCmdQuiet(parted_part)) {
    SendProgressUpdate(5, 0, "ERROR: Failed to create primary partition.");
    return false;
  }

  // For NTFS and FAT32, set msftdata/boot flags where applicable (helps Windows)
  if (filesystem == "ntfs") {
    RunCmdQuiet("sudo parted -s " + device_path + " set 1 msftdata on");
  } else if (filesystem == "fat32") {
    RunCmdQuiet("sudo parted -s " + device_path + " set 1 boot on");
  }
  SendProgressUpdate(5, 50, "Primary partition created.");

  std::string expected_partition;
  if (device_path.find("nvme") != std::string::npos) {
    expected_partition = device_path + "p1";
  } else {
    expected_partition = device_path + "1";
  }

  SendProgressUpdate(5, 55, "Informing kernel about partition changes...");
  for (int i = 0; i < 6; ++i) {
    RunCmdQuiet("sudo partprobe " + device_path);
    RunCmdQuiet("sudo udevadm trigger");
    RunCmdQuiet("sudo udevadm settle");
    if (access(expected_partition.c_str(), F_OK) == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }

  bool partition_exists = WaitForDeviceStable(expected_partition, 20, 300);
  if (!partition_exists) {
    SendProgressUpdate(5, 58, "WARNING: Partition node not visible; will attempt formatting fallback.");
  }

  std::string target = partition_exists ? expected_partition : device_path;

  // Ensure device/partition is stable before formatting
  if (!WaitForDeviceStable(target, 30, 300)) {
    SendProgressUpdate(5, 0, "ERROR: Device not stable for formatting.");
    return false;
  }

  // Format according to filesystem choice
  if (filesystem == "ntfs") {
    SendProgressUpdate(5, 60, std::string("Formatting as NTFS (Label: Code_wipe) on ") + target + " ...");
    std::string mkfs = "sudo mkfs.ntfs -F -f -L \"Code_wipe\" " + target;
    if (!RunCmdQuiet(mkfs)) {
      SendProgressUpdate(5, 0, "ERROR: NTFS format failed.");
      return false;
    }
  } else if (filesystem == "fat32") {
    SendProgressUpdate(5, 60, std::string("Formatting as FAT32 (Label: Code_wipe) on ") + target + " ...");
    // using mkfs.vfat -F 32 as requested
    std::string mkfs = "sudo mkfs.vfat -F 32 -n \"Code_wipe\" " + target;
    if (!RunCmdQuiet(mkfs)) {
      SendProgressUpdate(5, 0, "ERROR: FAT32 format failed.");
      return false;
    }
  } else {
    // fallback: NTFS
    SendProgressUpdate(5, 60, std::string("Formatting as NTFS (Label: Code_wipe) on ") + target + " ...");
    std::string mkfs = "sudo mkfs.ntfs -F -f -L \"Code_wipe\" " + target;
    if (!RunCmdQuiet(mkfs)) {
      SendProgressUpdate(5, 0, "ERROR: NTFS format failed.");
      return false;
    }
  }

  RunCmdQuiet("sudo partprobe " + device_path);
  RunCmdQuiet("sync");
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  SendProgressUpdate(5, 100, "Filesystem created successfully!");
  return true;
}

// -------------------- Wipe flows (accept device_type param) --------------------
static void PerformDoD522022MWipe(const std::string& device_path, const std::string& device_type = "Unknown") {
  g_wiping_active = true;
  auto start_time = std::chrono::steady_clock::now();
  auto start_epoch = std::time(nullptr);
  Log("==========================================");
  Log("Wipe Started at (epoch): %lld", (long long)start_epoch);
  Log("==========================================");

  char dbg[256];
  snprintf(dbg, sizeof(dbg), "Received device path: '%s'", device_path.c_str());
  SendProgressUpdate(0, 0, dbg);
  SendProgressUpdate(0, 0, "Initializing DoD 5220.22-M wipe...");

  uint64_t device_size = GetDiskSize(device_path);
  if (device_size == 0) {
    SendProgressUpdate(0, 0, "Error: Cannot determine device size.");
    g_wiping_active = false;
    return;
  }

  char size_str[128];
  snprintf(size_str, sizeof(size_str), "Device size: %.2f GB", device_size / (1024.0 * 1024.0 * 1024.0));
  SendProgressUpdate(0, 0, size_str);

  int fd = open(device_path.c_str(), O_WRONLY);
  if (fd < 0) {
    char em[256];
    snprintf(em, sizeof(em), "Error: Cannot open device for writing: %s (errno: %d)", strerror(errno), errno);
    SendProgressUpdate(0, 0, em);
    g_wiping_active = false;
    return;
  }

  SendProgressUpdate(1, 0, "Starting Pass 1: Writing zeros...");
  if (!OverwriteWithZeros(fd, device_size, 1, "DoD Pass 1: Writing zeros...")) {
    SendProgressUpdate(1, 0, g_wiping_active ? "Error in Pass 1" : "Wiping cancelled");
    close(fd);
    g_wiping_active = false;
    return;
  }
  SendProgressUpdate(1, 100, "Pass 1 completed: All zeros written");

  SendProgressUpdate(2, 0, "Starting Pass 2: Writing ones...");
  if (!OverwriteWithOnes(fd, device_size)) {
    SendProgressUpdate(2, 0, g_wiping_active ? "Error in Pass 2" : "Wiping cancelled");
    close(fd);
    g_wiping_active = false;
    return;
  }
  SendProgressUpdate(2, 100, "Pass 2 completed: All ones written");

  SendProgressUpdate(3, 0, "Starting Pass 3: Writing random data...");
  if (!OverwriteWithRandom(fd, device_size, 3, "DoD Pass 3: Writing random data...")) {
    SendProgressUpdate(3, 0, g_wiping_active ? "Error in Pass 3" : "Wiping cancelled");
    close(fd);
    g_wiping_active = false;
    return;
  }
  SendProgressUpdate(3, 100, "Pass 3 completed: Random data written");

  fsync(fd);
  close(fd);

  if (g_wiping_active)
    SendProgressUpdate(4, 100, "DoD 5220.22-M wipe completed successfully!");

  g_wiping_active = false;
  SendProgressUpdate(4, 0, "Recreating filesystem...");
  CreateNewFileSystem(device_path, device_type);
  SendProgressUpdate(6, 100, "DoD wipe + filesystem rebuild complete.");

  auto end_time = std::chrono::steady_clock::now();
  auto end_epoch = std::time(nullptr);
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

  Log("==========================================");
  Log("Wipe Finished at (epoch): %lld", (long long)end_epoch);
  Log("Total Time Taken: %lld seconds", (long long)elapsed);
  Log("==========================================");
}

// NIST Clear
static void PerformNISTClear(const std::string& device_path, const std::string& device_type = "Unknown") {
  g_wiping_active = true;
  auto start_time = std::chrono::steady_clock::now();
  auto start_epoch = std::time(nullptr);
  Log("==========================================");
  Log("Wipe Started at (epoch): %lld", (long long)start_epoch);
  Log("==========================================");

  SendProgressUpdate(0, 0, "Starting NIST SP 800-88 CLEAR...");

  uint64_t device_size = GetDiskSize(device_path);
  if (device_size == 0) {
    SendProgressUpdate(0, 0, "ERROR: Unable to read device size.");
    g_wiping_active = false;
    return;
  }

  int fd = open(device_path.c_str(), O_WRONLY);
  if (fd < 0) {
    SendProgressUpdate(0, 0, std::string("ERROR opening device: ") + strerror(errno));
    g_wiping_active = false;
    return;
  }

  SendProgressUpdate(1, 0, "NIST Clear Pass: Writing zeros...");
  if (!OverwriteWithZeros(fd, device_size, 1, "NIST: Zero-fill pass...")) {
    SendProgressUpdate(1, 0, "ERROR: Zero-fill failed.");
    close(fd);
    g_wiping_active = false;
    return;
  }
  SendProgressUpdate(1, 100, "Zero-fill complete.");
  fsync(fd);
  close(fd);

  SendProgressUpdate(4, 30, "Wiping partition metadata...");
  fd = open(device_path.c_str(), O_WRONLY);
  if (fd >= 0) {
    const size_t md = 1024 * 1024;
    std::vector<char> z(md, 0);

    lseek(fd, 0, SEEK_SET);
    write(fd, z.data(), md);
    fsync(fd);

    if (device_size > md) {
      lseek(fd, device_size - md, SEEK_SET);
      write(fd, z.data(), md);
      fsync(fd);
    }

    close(fd);
  }
  SendProgressUpdate(4, 60, "Metadata wiped.");

  SendProgressUpdate(4, 80, "Reloading partition table...");
  RunCmdQuiet(std::string("sudo partprobe ") + device_path);

  SendProgressUpdate(5, 0, "Recreating filesystem...");
  if (CreateNewFileSystem(device_path, device_type)) {
    SendProgressUpdate(6, 100, "NIST CLEAR completed successfully!");
  } else {
    SendProgressUpdate(6, 0, "NIST CLEAR complete, but filesystem creation failed.");
  }

  g_wiping_active = false;
  auto end_time = std::chrono::steady_clock::now();
  auto end_epoch = std::time(nullptr);
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

  Log("==========================================");
  Log("Wipe Finished at (epoch): %lld", (long long)end_epoch);
  Log("Total Time Taken: %lld seconds", (long long)elapsed);
  Log("==========================================");
}

// Gutmann (simplified 4-pass)
static void PerformGutmannWipe(const std::string& device_path, const std::string& device_type = "Unknown") {
  g_wiping_active = true;
  auto start_time = std::chrono::steady_clock::now();
  auto start_epoch = std::time(nullptr);
  Log("==========================================");
  Log("Wipe Started at (epoch): %lld", (long long)start_epoch);
  Log("==========================================");

  SendProgressUpdate(10, 0, "Starting Gutmann Method...");

  uint64_t size = GetDiskSize(device_path);
  if (size == 0) {
    SendProgressUpdate(10, 0, "ERROR: Unable to read disk size");
    g_wiping_active = false;
    return;
  }

  int fd = open(device_path.c_str(), O_WRONLY);
  if (fd < 0) {
    SendProgressUpdate(10, 0, "ERROR opening device");
    g_wiping_active = false;
    return;
  }

  SendProgressUpdate(10, 0, "Gutmann Pass 1: Random");
  if (!OverwriteRandomGutmann(fd, size, 10)) { close(fd); g_wiping_active = false; return; }
  SendProgressUpdate(10, 100, "Gutmann Pass 1 complete");

  SendProgressUpdate(11, 0, "Gutmann Pass 2: 0x55");
  if (!OverwritePattern(fd, size, 0x55, 11, "Gutmann Pass 2: 0x55")) { close(fd); g_wiping_active = false; return; }
  SendProgressUpdate(11, 100, "Gutmann Pass 2 complete");

  SendProgressUpdate(12, 0, "Gutmann Pass 3: 0xAA");
  if (!OverwritePattern(fd, size, 0xAA, 12, "Gutmann Pass 3: 0xAA")) { close(fd); g_wiping_active = false; return; }
  SendProgressUpdate(12, 100, "Gutmann Pass 3 complete");

  SendProgressUpdate(13, 0, "Gutmann Pass 4: Random");
  if (!OverwriteRandomGutmann(fd, size, 13)) { close(fd); g_wiping_active = false; return; }
  SendProgressUpdate(13, 100, "Gutmann Pass 4 complete");

  close(fd);

  SendProgressUpdate(13, 0, "Creating filesystem...");
  CreateNewFileSystem(device_path, device_type);
  SendProgressUpdate(13, 100, "Gutmann wipe complete.");
  g_wiping_active = false;
  auto end_time = std::chrono::steady_clock::now();
  auto end_epoch = std::time(nullptr);
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

  Log("==========================================");
  Log("Wipe Finished at (epoch): %lld", (long long)end_epoch);
  Log("Total Time Taken: %lld seconds", (long long)elapsed);
  Log("==========================================");
}

// Single Pass Zero
static void PerformSinglePassZero(const std::string& device_path, const std::string& device_type = "Unknown") {
  g_wiping_active = true;
  auto start_time = std::chrono::steady_clock::now();
  auto start_epoch = std::time(nullptr);
  Log("==========================================");
  Log("Wipe Started at (epoch): %lld", (long long)start_epoch);
  Log("==========================================");

  SendProgressUpdate(20, 0, "Starting Single Pass Zero...");

  uint64_t size = GetDiskSize(device_path);
  if (size == 0) {
    SendProgressUpdate(20, 0, "ERROR: Unable to read disk size");
    g_wiping_active = false;
    return;
  }

  int fd = open(device_path.c_str(), O_WRONLY);
  if (fd < 0) {
    SendProgressUpdate(20, 0, "ERROR opening device");
    g_wiping_active = false;
    return;
  }

  SendProgressUpdate(20, 0, "Single Pass: Writing zeros...");
  if (!OverwriteWithZeros(fd, size, 20, "Single Pass: Writing zeros...")) { close(fd); g_wiping_active = false; return; }
  SendProgressUpdate(20, 100, "Zero-fill complete");
  close(fd);

  SendProgressUpdate(21, 20, "Wiping metadata...");
  RunCmdQuiet("sudo dd if=/dev/zero of=" + device_path + " bs=1M count=1");
  SendProgressUpdate(21, 100, "Metadata wiped");

  SendProgressUpdate(22, 0, "Creating filesystem...");
  CreateNewFileSystem(device_path, device_type);
  SendProgressUpdate(22, 100, "Single pass zero complete.");
  g_wiping_active = false;
  auto end_time = std::chrono::steady_clock::now();
  auto end_epoch = std::time(nullptr);
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

  Log("==========================================");
  Log("Wipe Finished at (epoch): %lld", (long long)end_epoch);
  Log("Total Time Taken: %lld seconds", (long long)elapsed);
  Log("==========================================");
}

// -------------------- Combined entrypoint with safety --------------------
// Modified signature to accept device_type from Flutter
static void CompletelyWipeDisk(const std::string& device_path, const std::string& mode, const std::string& device_type = "Unknown") {

  // ------------------ CLEAN INPUT DEVICE NAME ------------------
  std::string cleaned = clean_dev(device_path);

  // ------------------ FIND ACTUAL SYSTEM BOOT DISK ------------------
  std::string boot_disk;
  {
    FILE* p = popen("df / | tail -1 | awk '{print $1}'", "r");
    if (p) {
      char buf[128];
      if (fgets(buf, sizeof(buf), p)) {
        boot_disk = clean_dev(buf);
      }
      pclose(p);
    }
  }

  // ------------------ BLOCK SYSTEM DISK ALWAYS ------------------
  if (cleaned == boot_disk) {
    SendProgressUpdate(0, 0, "❌ Refusing to wipe SYSTEM DISK!");
    return;
  }

  // ------------------ READ REMOVABLE FLAG ------------------
  std::string sys_rem = "/sys/block/" + cleaned + "/removable";
  std::ifstream f(sys_rem);
  int removable = 0;

  if (f.is_open()) {
    f >> removable;
    f.close();
  } else {
    SendProgressUpdate(0, 0, "WARNING: Could not read removable flag. Allowing ONLY if NOT system disk.");
    removable = 1;  // fallback: treat unknown devices as removable only if they are not boot-disk
  }

  // ------------------ BLOCK NON-REMOVABLE Nvme/SATA EXCEPT EXTERNAL HDD ------------------
  if (removable == 0) {
    SendProgressUpdate(0, 0, "Device marked non-removable (RM=0). Checking if safe...");

    // Allow external USB HDDs that incorrectly report RM=0 by checking transport
    bool is_usb = IsUsbDevice(cleaned);

    if (!is_usb) {
      SendProgressUpdate(0, 0, "❌ Refusing to wipe INTERNAL DISK (non-removable, non-USB).");
      return;
    }

    SendProgressUpdate(0, 0, "⚠ RM=0 but device TRAN=usb → Allowing external HDD wipe.");
  }

  // ------------------ SELECT WIPE MODE ------------------
  if (mode == "nist")
    PerformNISTClear(device_path, device_type);
  else if (mode == "gutmann")
    PerformGutmannWipe(device_path, device_type);
  else if (mode == "singlepass")
    PerformSinglePassZero(device_path, device_type);
  else
    PerformDoD522022MWipe(device_path, device_type);
}

// -------------------- Method channel handler --------------------
static void method_call_handler(FlMethodChannel* channel, FlMethodCall* method_call, gpointer user_data) {
  const gchar* method = fl_method_call_get_name(method_call);

  if (g_strcmp0(method, "getDiskInfo") == 0) {
    std::vector<std::string> disk_info = GetDiskInfo();
    g_autoptr(FlValue) result = fl_value_new_list();
    for (const auto& d : disk_info) fl_value_append_take(result, fl_value_new_string(d.c_str()));
    fl_method_call_respond_success(method_call, result, nullptr);

  } else if (g_strcmp0(method, "startDoD522022MWipe") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    FlValue* dv = args ? fl_value_lookup_string(args, "devicePath") : nullptr;
    const char* device_path_c = dv ? fl_value_get_string(dv) : nullptr;
    if (!device_path_c) {
      fl_method_call_respond_error(method_call, "INVALID_ARGS", "Missing devicePath", nullptr, nullptr);
      return;
    }
    std::string device_path(device_path_c);

    if (g_wiping_active) {
      fl_method_call_respond_error(method_call, "WIPE_IN_PROGRESS", "Another wipe operation is already in progress", nullptr, nullptr);
      return;
    }
    std::thread t([device_path]() { PerformDoD522022MWipe(device_path); });
    t.detach();
    fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);

  } else if (g_strcmp0(method, "completelyWipeDisk") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    FlValue* dv = args ? fl_value_lookup_string(args, "devicePath") : nullptr;
    const char* device_path_c = dv ? fl_value_get_string(dv) : nullptr;
    if (!device_path_c) {
      fl_method_call_respond_error(method_call, "INVALID_ARGS", "Missing devicePath", nullptr, nullptr);
      return;
    }
    std::string device_path(device_path_c);

    std::string sys_rem = "/sys/block/";
    std::string dev_basename = device_path;
    if (dev_basename.rfind("/dev/", 0) == 0) dev_basename = dev_basename.substr(5);
    while (!dev_basename.empty() && std::isdigit(dev_basename.back())) dev_basename.pop_back();
    if (!dev_basename.empty() && dev_basename.back() == 'p') dev_basename.pop_back();

    std::ifstream f2(sys_rem + dev_basename + "/removable");
    int removable = 0;
    if (f2.is_open()) {
      f2 >> removable;
      f2.close();
    } else {
      // fallback to transport
      bool usb_fallback = IsUsbDevice(dev_basename);
      if (!usb_fallback) {
        fl_method_call_respond_error(method_call, "SYSFS_READ_FAILED", "Unable to determine if device is removable and transport is not USB", nullptr, nullptr);
        return;
      }
      removable = 1;
    }

    if (removable == 0) {
      if (!IsUsbDevice(dev_basename)) {
        fl_method_call_respond_error(method_call, "SYSTEM_DISK_BLOCKED", "Refusing to wipe internal system disk", nullptr, nullptr);
        return;
      }
    }

    FlValue* mode_val = args ? fl_value_lookup_string(args, "mode") : nullptr;
    const char* mode_c = mode_val ? fl_value_get_string(mode_val) : nullptr;
    std::string mode = mode_c ? mode_c : "dod";

    // Read deviceType passed from Flutter
    FlValue* dtype_val = args ? fl_value_lookup_string(args, "deviceType") : nullptr;
    const char* device_type_c = dtype_val ? fl_value_get_string(dtype_val) : nullptr;
    std::string device_type = device_type_c ? device_type_c : "Unknown";

    std::thread t([device_path, mode, device_type]() { CompletelyWipeDisk(device_path, mode, device_type); });
    t.detach();
    fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);

  } else if (g_strcmp0(method, "cancelWipe") == 0) {
    g_wiping_active = false;
    SendProgressUpdate(0, 0, "Wipe operation cancelled by user");
    fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);
  } else {
    fl_method_call_respond_not_implemented(method_call, nullptr);
  }
}

// -------------------- GApplication callbacks --------------------
static void my_application_activate(GApplication* application) {
  MyApplication* self = MY_APPLICATION(application);
  GtkWindow* window = GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application)));

  gboolean use_header_bar = TRUE;
#ifdef GDK_WINDOWING_X11
  GdkScreen* screen = gtk_window_get_screen(window);
  if (GDK_IS_X11_SCREEN(screen)) {
    const gchar* wm_name = gdk_x11_screen_get_window_manager_name(screen);
    if (g_strcmp0(wm_name, "GNOME Shell") != 0) {
      use_header_bar = FALSE;
    }
  }
#endif
  if (use_header_bar) {
    GtkHeaderBar* header_bar = GTK_HEADER_BAR(gtk_header_bar_new());
    gtk_widget_show(GTK_WIDGET(header_bar));
    gtk_header_bar_set_title(header_bar, "CODEWIPE");
    gtk_header_bar_set_show_close_button(header_bar, TRUE);
    gtk_window_set_titlebar(window, GTK_WIDGET(header_bar));
  } else {
    gtk_window_set_title(window, "CODEWIPE");
  }

  gtk_window_set_default_size(window, 1280, 720);
  gtk_widget_show(GTK_WIDGET(window));

  g_autoptr(FlDartProject) project = fl_dart_project_new();
  fl_dart_project_set_dart_entrypoint_arguments(project, self->dart_entrypoint_arguments);

  FlView* view = fl_view_new(project);
  gtk_widget_show(GTK_WIDGET(view));
  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

  fl_register_plugins(FL_PLUGIN_REGISTRY(view));

  FlEngine* engine = fl_view_get_engine(view);
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  FlMethodChannel* method_channel = fl_method_channel_new(
      fl_engine_get_binary_messenger(engine),
      "com.yourdomain.deviceinfo",
      FL_METHOD_CODEC(codec));

  g_method_channel = method_channel;
  self->method_channel = method_channel;

  fl_method_channel_set_method_call_handler(method_channel, method_call_handler, self, nullptr);

  gtk_widget_grab_focus(GTK_WIDGET(view));
}

static gboolean my_application_local_command_line(GApplication* application, gchar*** arguments, int* exit_status) {
  MyApplication* self = MY_APPLICATION(application);
  self->dart_entrypoint_arguments = g_strdupv(*arguments + 1);

  g_autoptr(GError) error = nullptr;
  if (!g_application_register(application, nullptr, &error)) {
     g_warning("Failed to register: %s", error->message);
     *exit_status = 1;
     return TRUE;
  }

  g_application_activate(application);
  *exit_status = 0;
  return TRUE;
}

static void my_application_startup(GApplication* application) {
  G_APPLICATION_CLASS(my_application_parent_class)->startup(application);
}
static void my_application_shutdown(GApplication* application) {
  G_APPLICATION_CLASS(my_application_parent_class)->shutdown(application);
}
static void my_application_dispose(GObject* object) {
  MyApplication* self = MY_APPLICATION(object);
  g_clear_pointer(&self->dart_entrypoint_arguments, g_strfreev);
  G_OBJECT_CLASS(my_application_parent_class)->dispose(object);
}
static void my_application_class_init(MyApplicationClass* klass) {
  G_APPLICATION_CLASS(klass)->activate = my_application_activate;
  G_APPLICATION_CLASS(klass)->local_command_line = my_application_local_command_line;
  G_APPLICATION_CLASS(klass)->startup = my_application_startup;
  G_APPLICATION_CLASS(klass)->shutdown = my_application_shutdown;
  G_OBJECT_CLASS(my_application_parent_class)->dispose = my_application_dispose;
}
static void my_application_init(MyApplication* self) {}
MyApplication* my_application_new() {
  return MY_APPLICATION(g_object_new(my_application_get_type(),
                                     "application-id", APPLICATION_ID,
                                     "flags", G_APPLICATION_NON_UNIQUE,
                                     nullptr));
}

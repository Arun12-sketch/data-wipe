#include "my_application.h"

#include <flutter_linux/flutter_linux.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "flutter/generated_plugin_registrant.h"

#include <cstdio>
#include <cstdlib>
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

// Only define APPLICATION_ID if not already defined
#ifndef APPLICATION_ID
#define APPLICATION_ID "com.example.code_wipe"
#endif

struct _MyApplication {
  GtkApplication parent_instance;
  char** dart_entrypoint_arguments;
  FlMethodChannel* method_channel;
};

G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION)

// Global variables for progress reporting
static FlMethodChannel* g_method_channel = nullptr;
static bool g_wiping_active = false;

// Enhanced function to get disk size in bytes using multiple methods
static uint64_t GetDiskSize(const std::string& device_path) {
    printf("Attempting to get size for device: %s\n", device_path.c_str());
    
    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        printf("Failed to open device %s: %s (errno: %d)\n", 
               device_path.c_str(), strerror(errno), errno);
        
        // Try alternative sysfs method
        std::string device_name = device_path.substr(device_path.find_last_of('/') + 1);
        std::string sysfs_path = "/sys/class/block/" + device_name + "/size";
        
        printf("Trying sysfs method: %s\n", sysfs_path.c_str());
        
        std::ifstream size_file(sysfs_path);
        if (size_file.is_open()) {
            uint64_t sectors;
            size_file >> sectors;
            size_file.close();
            uint64_t size_bytes = sectors * 512;
            printf("Got size from sysfs: %lu bytes\n", size_bytes);
            return size_bytes;
        } else {
            printf("Sysfs method also failed\n");
        }
        
        return 0;
    }
    
    uint64_t size = 0;
    
    // Method 1: Use BLKGETSIZE64 ioctl
    printf("Trying BLKGETSIZE64 ioctl...\n");
    if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
        close(fd);
        printf("BLKGETSIZE64 succeeded: %lu bytes\n", size);
        return size;
    } else {
        printf("BLKGETSIZE64 failed: %s (errno: %d)\n", strerror(errno), errno);
    }
    
    // Method 2: Use BLKGETSIZE ioctl and multiply by 512
    printf("Trying BLKGETSIZE ioctl...\n");
    unsigned long sectors;
    if (ioctl(fd, BLKGETSIZE, &sectors) == 0) {
        close(fd);
        size = (uint64_t)sectors * 512;
        printf("BLKGETSIZE succeeded: %lu sectors = %lu bytes\n", sectors, size);
        return size;
    } else {
        printf("BLKGETSIZE failed: %s (errno: %d)\n", strerror(errno), errno);
    }
    
    // Method 3: Try fstat
    printf("Trying fstat method...\n");
    struct stat st;
    if (fstat(fd, &st) == 0) {
        printf("fstat succeeded, checking file type...\n");
        if (S_ISREG(st.st_mode)) {
            size = st.st_size;
            printf("Regular file size: %lu bytes\n", size);
        } else if (S_ISBLK(st.st_mode)) {
            printf("Block device detected, trying lseek...\n");
            off_t end = lseek(fd, 0, SEEK_END);
            if (end != -1) {
                size = end;
                lseek(fd, 0, SEEK_SET);
                printf("lseek size: %lu bytes\n", size);
            } else {
                printf("lseek failed: %s (errno: %d)\n", strerror(errno), errno);
            }
        }
    } else {
        printf("fstat failed: %s (errno: %d)\n", strerror(errno), errno);
    }
    
    close(fd);
    
    printf("Final size result: %lu bytes\n", size);
    return size;
}

// Function to get dynamic disk info from Linux system using lsblk
static std::vector<std::string> GetDiskInfo() {
    std::vector<std::string> disks;
    
    const char* cmd = "lsblk -o NAME,SIZE,MOUNTPOINT --noheadings";
    
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        disks.push_back("Error: Cannot get disk info");
        return disks;
    }
    
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        
        // Remove trailing newline
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        
        if (!line.empty()) {
            disks.push_back(line);
        }
    }
    pclose(pipe);
    
    if (disks.empty()) {
        disks.push_back("No disk information available");
    }
    
    return disks;
}

// Function to send progress updates to Flutter
static void SendProgressUpdate(int pass, int progress, const std::string& status) {
    if (!g_method_channel) return;
    
    g_autoptr(FlValue) result = fl_value_new_map();
    fl_value_set_string_take(result, "pass", fl_value_new_int(pass));
    fl_value_set_string_take(result, "progress", fl_value_new_int(progress));
    fl_value_set_string_take(result, "status", fl_value_new_string(status.c_str()));
    
    fl_method_channel_invoke_method(g_method_channel, "onWipeProgress", result, nullptr, nullptr, nullptr);
}

// DoD 5220.22-M Pass 1: Write zeros
static bool OverwriteWithZeros(int fd, uint64_t size) {
    const size_t buffer_size = 1024 * 1024; // 1MB buffer
    std::vector<char> buffer(buffer_size, 0x00);
    
    lseek(fd, 0, SEEK_SET);
    uint64_t bytes_written = 0;
    
    while (bytes_written < size && g_wiping_active) {
        size_t to_write = std::min(buffer_size, size - bytes_written);
        ssize_t written = write(fd, buffer.data(), to_write);
        
        if (written <= 0) return false;
        
        bytes_written += written;
        int progress = (int)((bytes_written * 100) / size);
        
        if (bytes_written % (buffer_size * 10) == 0) { // Update every 10MB
            SendProgressUpdate(1, progress, "Pass 1: Writing zeros...");
        }
        
        // Sync every 100MB for progress
        if (bytes_written % (buffer_size * 100) == 0) {
            fsync(fd);
        }
        
        // Small delay to prevent overwhelming the system
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    fsync(fd);
    return g_wiping_active;
}

// DoD 5220.22-M Pass 2: Write ones
static bool OverwriteWithOnes(int fd, uint64_t size) {
    const size_t buffer_size = 1024 * 1024; // 1MB buffer
    std::vector<char> buffer(buffer_size, (char)0xFF);
    
    lseek(fd, 0, SEEK_SET);
    uint64_t bytes_written = 0;
    
    while (bytes_written < size && g_wiping_active) {
        size_t to_write = std::min(buffer_size, size - bytes_written);
        ssize_t written = write(fd, buffer.data(), to_write);
        
        if (written <= 0) return false;
        
        bytes_written += written;
        int progress = (int)((bytes_written * 100) / size);
        
        if (bytes_written % (buffer_size * 10) == 0) { // Update every 10MB
            SendProgressUpdate(2, progress, "Pass 2: Writing ones...");
        }
        
        // Sync every 100MB for progress
        if (bytes_written % (buffer_size * 100) == 0) {
            fsync(fd);
        }
        
        // Small delay to prevent overwhelming the system
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    fsync(fd);
    return g_wiping_active;
}

// DoD 5220.22-M Pass 3: Write random data
static bool OverwriteWithRandom(int fd, uint64_t size) {
    const size_t buffer_size = 1024 * 1024; // 1MB buffer
    std::vector<char> buffer(buffer_size);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    lseek(fd, 0, SEEK_SET);
    uint64_t bytes_written = 0;
    
    while (bytes_written < size && g_wiping_active) {
        size_t to_write = std::min(buffer_size, size - bytes_written);
        
        // Fill buffer with random data
        for (size_t i = 0; i < to_write; ++i) {
            buffer[i] = static_cast<char>(dis(gen));
        }
        
        ssize_t written = write(fd, buffer.data(), to_write);
        
        if (written <= 0) return false;
        
        bytes_written += written;
        int progress = (int)((bytes_written * 100) / size);
        
        if (bytes_written % (buffer_size * 10) == 0) { // Update every 10MB
            SendProgressUpdate(3, progress, "Pass 3: Writing random data...");
        }
        
        // Sync every 100MB for progress
        if (bytes_written % (buffer_size * 100) == 0) {
            fsync(fd);
        }
        
        // Small delay to prevent overwhelming the system
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    fsync(fd);
    return g_wiping_active;
}

// NEW FUNCTION: Create a new filesystem after wiping
static bool CreateNewFileSystem(const std::string& device_path) {
    SendProgressUpdate(5, 0, "Creating new partition table...");
    
    // Create a new GPT partition table
    std::string parted_cmd = "parted -s " + device_path + " mklabel gpt";
    int result = system(parted_cmd.c_str());
    if (result != 0) {
        SendProgressUpdate(5, 0, "Warning: Failed to create GPT table, trying MBR...");
        // Try MBR if GPT fails
        parted_cmd = "parted -s " + device_path + " mklabel msdos";
        result = system(parted_cmd.c_str());
        if (result != 0) {
            SendProgressUpdate(5, 0, "Error: Failed to create partition table");
            return false;
        }
    }
    
    SendProgressUpdate(5, 25, "Partition table created successfully");
    
    // Create a primary partition using all available space
    SendProgressUpdate(5, 30, "Creating primary partition...");
    parted_cmd = "parted -s " + device_path + " mkpart primary 1MiB 100%";
    result = system(parted_cmd.c_str());
    if (result != 0) {
        SendProgressUpdate(5, 0, "Error: Failed to create primary partition");
        return false;
    }
    
    SendProgressUpdate(5, 50, "Primary partition created");
    
    // Wait a moment for the kernel to recognize the new partition
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // Force kernel to re-read partition table
    std::string partprobe_cmd = "partprobe " + device_path;
    system(partprobe_cmd.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Determine the partition device path (e.g., /dev/sda1)
    std::string partition_path = device_path;
    if (device_path.back() >= '0' && device_path.back() <= '9') {
        partition_path += "p1";  // For devices like /dev/nvme0n1 -> /dev/nvme0n1p1
    } else {
        partition_path += "1";   // For devices like /dev/sda -> /dev/sda1
    }
    
    // Format with FAT32 filesystem (compatible with most systems)
    SendProgressUpdate(5, 60, "Formatting with FAT32 filesystem...");
    std::string mkfs_cmd = "mkfs.fat -F 32 -n \"CLEAN_DISK\" " + partition_path + " 2>/dev/null";
    result = system(mkfs_cmd.c_str());
    
    if (result != 0) {
        // If FAT32 fails, try ext4
        SendProgressUpdate(5, 70, "FAT32 failed, trying ext4 filesystem...");
        mkfs_cmd = "mkfs.ext4 -F -L \"CLEAN_DISK\" " + partition_path + " 2>/dev/null";
        result = system(mkfs_cmd.c_str());
        
        if (result != 0) {
            SendProgressUpdate(5, 0, "Error: Failed to format partition");
            return false;
        }
        SendProgressUpdate(5, 90, "ext4 filesystem created successfully");
    } else {
        SendProgressUpdate(5, 90, "FAT32 filesystem created successfully");
    }
    
    // Final partition table update
    system(("partprobe " + device_path + " 2>/dev/null").c_str());
    
    SendProgressUpdate(5, 100, "New filesystem created - disk is ready to use!");
    return true;
}

// Main DoD 5220.22-M wiping function with enhanced error reporting
static void PerformDoD522022MWipe(const std::string& device_path) {
    g_wiping_active = true;
    
    // Debug: Log the exact device path received
    char debug_msg[256];
    snprintf(debug_msg, sizeof(debug_msg), "Received device path: '%s'", device_path.c_str());
    SendProgressUpdate(0, 0, debug_msg);
    
    SendProgressUpdate(0, 0, "Initializing DoD 5220.22-M wipe...");
    
    // Get device size
    uint64_t device_size = GetDiskSize(device_path);
    if (device_size == 0) {
        SendProgressUpdate(0, 0, "Error: Cannot determine device size. Check permissions and device path.");
        g_wiping_active = false;
        return;
    }
    
    char size_str[64];
    snprintf(size_str, sizeof(size_str), "Device size: %.2f GB", device_size / (1024.0 * 1024.0 * 1024.0));
    SendProgressUpdate(0, 0, size_str);
    
    // Open the actual device for writing
    int fd = open(device_path.c_str(), O_WRONLY);
    if (fd < 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Error: Cannot open device for writing: %s (errno: %d)", 
                strerror(errno), errno);
        SendProgressUpdate(0, 0, error_msg);
        g_wiping_active = false;
        return;
    }
    
    // Pass 1: Zeros
    SendProgressUpdate(1, 0, "Starting Pass 1: Writing zeros...");
    if (!OverwriteWithZeros(fd, device_size)) {
        SendProgressUpdate(1, 0, g_wiping_active ? "Error in Pass 1" : "Wiping cancelled");
        close(fd);
        g_wiping_active = false;
        return;
    }
    SendProgressUpdate(1, 100, "Pass 1 completed: All zeros written");
    
    // Pass 2: Ones
    SendProgressUpdate(2, 0, "Starting Pass 2: Writing ones...");
    if (!OverwriteWithOnes(fd, device_size)) {
        SendProgressUpdate(2, 0, g_wiping_active ? "Error in Pass 2" : "Wiping cancelled");
        close(fd);
        g_wiping_active = false;
        return;
    }
    SendProgressUpdate(2, 100, "Pass 2 completed: All ones written");
    
    // Pass 3: Random
    SendProgressUpdate(3, 0, "Starting Pass 3: Writing random data...");
    if (!OverwriteWithRandom(fd, device_size)) {
        SendProgressUpdate(3, 0, g_wiping_active ? "Error in Pass 3" : "Wiping cancelled");
        close(fd);
        g_wiping_active = false;
        return;
    }
    SendProgressUpdate(3, 100, "Pass 3 completed: Random data written");
    
    // Final sync
    fsync(fd);
    close(fd);
    
    if (g_wiping_active) {
        SendProgressUpdate(4, 100, "DoD 5220.22-M wipe completed successfully!");
    }
    
    g_wiping_active = false;
}

// Complete disk wipe function - FIXED VERSION
static void CompletelyWipeDisk(const std::string& device_path) {
    g_wiping_active = true;
    
    SendProgressUpdate(0, 0, "Starting Complete Disk Wipe (DoD + File Removal + Filesystem Recreation)...");
    
    // Step 1: Perform DoD 5220.22-M wipe (existing algorithm)
    SendProgressUpdate(0, 0, "Step 1: DoD 5220.22-M data overwriting...");
    
    // Get device size first
    uint64_t device_size = GetDiskSize(device_path);
    if (device_size == 0) {
        SendProgressUpdate(0, 0, "Error: Cannot determine device size. Check permissions and device path.");
        g_wiping_active = false;
        return;
    }
    
    char size_str[64];
    snprintf(size_str, sizeof(size_str), "Device size: %.2f GB", device_size / (1024.0 * 1024.0 * 1024.0));
    SendProgressUpdate(0, 0, size_str);
    
    // Open device for DoD wiping
    int fd = open(device_path.c_str(), O_WRONLY);
    if (fd < 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Error: Cannot open device for writing: %s (errno: %d)", 
                strerror(errno), errno);
        SendProgressUpdate(0, 0, error_msg);
        g_wiping_active = false;
        return;
    }
    
    // DoD Pass 1: Zeros
    SendProgressUpdate(1, 0, "DoD Pass 1: Writing zeros...");
    if (!OverwriteWithZeros(fd, device_size)) {
        SendProgressUpdate(1, 0, g_wiping_active ? "Error in DoD Pass 1" : "Wiping cancelled");
        close(fd);
        g_wiping_active = false;
        return;
    }
    SendProgressUpdate(1, 100, "DoD Pass 1 completed");
    
    // DoD Pass 2: Ones
    SendProgressUpdate(2, 0, "DoD Pass 2: Writing ones...");
    if (!OverwriteWithOnes(fd, device_size)) {
        SendProgressUpdate(2, 0, g_wiping_active ? "Error in DoD Pass 2" : "Wiping cancelled");
        close(fd);
        g_wiping_active = false;
        return;
    }
    SendProgressUpdate(2, 100, "DoD Pass 2 completed");
    
    // DoD Pass 3: Random
    SendProgressUpdate(3, 0, "DoD Pass 3: Writing random data...");
    if (!OverwriteWithRandom(fd, device_size)) {
        SendProgressUpdate(3, 0, g_wiping_active ? "Error in DoD Pass 3" : "Wiping cancelled");
        close(fd);
        g_wiping_active = false;
        return;
    }
    SendProgressUpdate(3, 100, "DoD Pass 3 completed");
    
    close(fd);
    
    if (!g_wiping_active) return;
    
    // Step 2: Destroy partition table and filesystem metadata
    SendProgressUpdate(4, 0, "Step 2: Destroying old partition table and filesystem metadata...");
    
    fd = open(device_path.c_str(), O_WRONLY);
    if (fd >= 0) {
        // Wipe first 1MB (contains partition table, boot sectors, filesystem headers)
        const size_t metadata_size = 1024 * 1024; // 1MB
        std::vector<char> zero_buffer(metadata_size, 0x00);
        
        lseek(fd, 0, SEEK_SET);
        ssize_t written_start = write(fd, zero_buffer.data(), metadata_size);
        fsync(fd);
        
        // Also wipe last 1MB (backup partition tables)
        if (device_size > metadata_size) {
            lseek(fd, device_size - metadata_size, SEEK_SET);
            write(fd, zero_buffer.data(), metadata_size);
            fsync(fd);
        }
        
        close(fd);
        
        if (written_start > 0) {
            SendProgressUpdate(4, 50, "Old partition table destroyed");
        }
    }
    
    // Step 3: Force kernel to re-read partition table
    SendProgressUpdate(4, 75, "Step 3: Updating system partition table...");
    std::string partprobe_cmd = "partprobe " + device_path + " 2>/dev/null";
    system(partprobe_cmd.c_str());
    
    SendProgressUpdate(4, 100, "Old filesystem completely destroyed");
    
    if (!g_wiping_active) return;
    
    // Step 4: NEW - Create new filesystem so disk is usable
    if (CreateNewFileSystem(device_path)) {
        SendProgressUpdate(6, 100, "Complete disk wipe finished! Disk has been securely wiped and is ready for use.");
    } else {
        SendProgressUpdate(6, 100, "Disk wipe completed but filesystem creation failed. Disk may need manual formatting.");
    }
    
    g_wiping_active = false;
}

// Platform channel method call handler
static void method_call_handler(FlMethodChannel* channel, FlMethodCall* method_call, gpointer user_data) {
    const gchar* method = fl_method_call_get_name(method_call);
    
    if (g_strcmp0(method, "getDiskInfo") == 0) {
        // Get disk information
        std::vector<std::string> disk_info = GetDiskInfo();
        
        // Create FlValue list
        g_autoptr(FlValue) result = fl_value_new_list();
        for (const auto& disk : disk_info) {
            fl_value_append_take(result, fl_value_new_string(disk.c_str()));
        }
        
        // Send success response
        fl_method_call_respond_success(method_call, result, nullptr);
        
    } else if (g_strcmp0(method, "startDoD522022MWipe") == 0) {
        // Get arguments
        FlValue* args = fl_method_call_get_args(method_call);
        const char* device_path = fl_value_get_string(fl_value_lookup_string(args, "devicePath"));
        
        if (g_wiping_active) {
            fl_method_call_respond_error(method_call, "WIPE_IN_PROGRESS", "Another wipe operation is already in progress", nullptr, nullptr);
            return;
        }
        
        // Start wiping in a separate thread
        std::thread wipe_thread([device_path]() {
            PerformDoD522022MWipe(device_path);
        });
        wipe_thread.detach();
        
        fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);
        
    } else if (g_strcmp0(method, "completelyWipeDisk") == 0) {
        // Complete disk wipe method
        FlValue* args = fl_method_call_get_args(method_call);
        const char* device_path = fl_value_get_string(fl_value_lookup_string(args, "devicePath"));
        
        if (g_wiping_active) {
            fl_method_call_respond_error(method_call, "WIPE_IN_PROGRESS", "Another wipe operation is already in progress", nullptr, nullptr);
            return;
        }
        
        // Start complete wiping in a separate thread
        std::thread wipe_thread([device_path]() {
            CompletelyWipeDisk(device_path);
        });
        wipe_thread.detach();
        
        fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);
        
    } else if (g_strcmp0(method, "cancelWipe") == 0) {
        g_wiping_active = false;
        SendProgressUpdate(0, 0, "Wipe operation cancelled by user");
        fl_method_call_respond_success(method_call, fl_value_new_bool(TRUE), nullptr);
        
    } else {
        // Method not implemented
        fl_method_call_respond_not_implemented(method_call, nullptr);
    }
}

// Implements GApplication::activate.
static void my_application_activate(GApplication* application) {
  MyApplication* self = MY_APPLICATION(application);
  GtkWindow* window =
      GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application)));

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
  
  // Store reference for progress updates
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
  G_OBJECT_CLASS(klass)->dispose = my_application_dispose;
}

static void my_application_init(MyApplication* self) {}

MyApplication* my_application_new() {
  return MY_APPLICATION(g_object_new(my_application_get_type(),
                                     "application-id", APPLICATION_ID,
                                     "flags", G_APPLICATION_NON_UNIQUE,
                                     nullptr));
}
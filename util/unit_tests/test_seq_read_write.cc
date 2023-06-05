//
// Created by chiro on 23-6-5.
//

#include <filesystem>
#include <string>

#include "../tools/tools.h"
#include "fs/fs_aquafs.h"

using namespace aquafs;

int test_seq_read_write(int dev_num, const char* fs_uri, uint64_t kib) {
  prepare_test_env(dev_num);
  aquafs_tools_call({"mkfs", fs_uri, "--aux_path=/tmp/aux_path", "--force"});
  auto data_source_dir = std::filesystem::temp_directory_path() / "aquafs_test";
  system((std::string("rm -rf ") + data_source_dir.string()).c_str());
  std::filesystem::create_directories(data_source_dir);
  auto filename = "test_file";
  auto file = data_source_dir / filename;
  system((std::string("dd if=/dev/random of=") + file.string() +
          " bs=1K count=" + std::to_string(kib))
             .c_str());
  // calculate checksum
  size_t file_hash = get_file_hash(file);
  printf("file hash: %zx\n", file_hash);
  // call restore
  aquafs_tools_call({"restore", fs_uri, "--path=" + data_source_dir.string()});

  auto dump_dir = std::filesystem::temp_directory_path() / "aquafs_dump";
  system((std::string("rm -rf ") + dump_dir.string()).c_str());
  std::filesystem::create_directories(dump_dir);
  aquafs_tools_call({"backup", fs_uri, "--path=" + dump_dir.string()});
  sleep(1);
  // calculate checksum again
  auto backup_file = dump_dir / filename;
  assert(std::filesystem::exists(backup_file));
  size_t file_hash2 = get_file_hash(backup_file);
  // system((std::string("file ") + file.string() + " " + backup_file.string())
  //            .c_str());
  system((std::string("md5sum ") + file.string() + " " + backup_file.string())
             .c_str());
  printf("file hash2: %zx\n", file_hash2);
  fflush(stdout);
  assert(file_hash == file_hash2);
  return 0;
}

int main() {
  auto sz = 128l * 1024;
  // test_seq_read_write(1, "--zbd=nullb0", sz);
  // test_seq_read_write(
  //     4, "--raids=raida:dev:nullb0,dev:nullb1,dev:nullb2,dev:nullb3", sz);
  test_seq_read_write(
      4, "--raids=raid0:dev:nullb0,dev:nullb1,dev:nullb2,dev:nullb3", sz);
}
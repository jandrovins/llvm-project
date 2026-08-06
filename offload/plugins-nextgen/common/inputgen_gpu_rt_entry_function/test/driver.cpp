#include <omptarget.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef INPUTGEN_GPU_ARCH
#define INPUTGEN_GPU_ARCH "unknown"
#endif

#define INPUTGEN_MODE_GENERATE 1
#define INPUTGEN_MODE_REPLAY 2
#define OMP_KERNEL_ARG_VERSION 5

static const char *InputFile = "input.txt";
static const char *EntryName = "__ig_entry_vvv_foo";

static std::vector<char> readFile(const std::string &Path) {
  std::ifstream File(Path, std::ios::binary | std::ios::ate);
  if (!File) {
    std::fprintf(stderr, "error: failed to open %s\n", Path.c_str());
    std::exit(1);
  }

  std::streamsize Size = File.tellg();
  if (Size <= 0) {
    std::fprintf(stderr, "error: empty image file %s\n", Path.c_str());
    std::exit(1);
  }

  std::vector<char> Bytes(static_cast<size_t>(Size));
  File.seekg(0, std::ios::beg);
  if (!File.read(Bytes.data(), Size)) {
    std::fprintf(stderr, "error: failed to read %s\n", Path.c_str());
    std::exit(1);
  }
  return Bytes;
}

int main(int argc, char **argv) {
  if (argc != 2 ||
      (std::strcmp(argv[1], "generate") != 0 &&
       std::strcmp(argv[1], "replay") != 0)) {
    std::fprintf(stderr, "usage: %s generate|replay\n", argv[0]);
    return 1;
  }

  int Mode = std::strcmp(argv[1], "generate") == 0 ? INPUTGEN_MODE_GENERATE
                                                     : INPUTGEN_MODE_REPLAY;

  std::vector<char> Image =
      readFile(std::string("build/test.") + INPUTGEN_GPU_ARCH + ".image");

  llvm::offloading::EntryTy OffloadEntries[] = {
      {0x0, 0x1, llvm::object::OffloadKind::OFK_OpenMP, 0, (void *)0x1,
       const_cast<char *>(EntryName), 0, 0, nullptr}};

  __tgt_device_image DeviceImage;
  DeviceImage.ImageStart = Image.data();
  DeviceImage.ImageEnd = Image.data() + Image.size();
  DeviceImage.EntriesBegin = &OffloadEntries[0];
  DeviceImage.EntriesEnd = &OffloadEntries[1];

  __tgt_bin_desc Desc;
  Desc.NumDeviceImages = 1;
  Desc.DeviceImages = &DeviceImage;
  Desc.HostEntriesBegin = &OffloadEntries[0];
  Desc.HostEntriesEnd = &OffloadEntries[1];

  __tgt_register_lib(&Desc);

  int Device = 0;
  int Host = omp_get_initial_device();

  void *DBuf = omp_target_alloc(sizeof(int), Device);
  void *DRes = omp_target_alloc(sizeof(int), Device);
  if (!DBuf || !DRes) {
    std::fprintf(stderr, "error: omp_target_alloc failed\n");
    if (DBuf)
      omp_target_free(DBuf, Device);
    if (DRes)
      omp_target_free(DRes, Device);
    return 1;
  }

  if (Mode == INPUTGEN_MODE_REPLAY) {
    FILE *F = std::fopen(InputFile, "r");
    if (!F) {
      std::fprintf(stderr, "error: failed to open %s for replay\n", InputFile);
      omp_target_free(DBuf, Device);
      omp_target_free(DRes, Device);
      return 1;
    }
    int ReplayValue = 0;
    int Scanned = std::fscanf(F, "%d", &ReplayValue);
    std::fclose(F);
    if (Scanned != 1) {
      std::fprintf(stderr, "error: failed to read replay value from %s\n",
                   InputFile);
      omp_target_free(DBuf, Device);
      omp_target_free(DRes, Device);
      return 1;
    }

    if (omp_target_memcpy(DBuf, &ReplayValue, sizeof(int), 0, 0, Device,
                          Host) != 0) {
      std::fprintf(stderr, "error: omp_target_memcpy to device failed\n");
      omp_target_free(DBuf, Device);
      omp_target_free(DRes, Device);
      return 1;
    }
  }

  int32_t KernelMode = Mode;
  int64_t Size = sizeof(int);
  void *ArgPtrs[] = {&KernelMode, &DBuf, &Size, &DRes};
  void *ArgBasePtrs[] = {&KernelMode, &DBuf, &Size, &DRes};
  int64_t ArgSizes[] = {sizeof(KernelMode), sizeof(DBuf), sizeof(Size),
                        sizeof(DRes)};
  int64_t ArgTypes[] = {
      OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL,
      OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL,
      OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL,
      OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL};

  KernelArgsTy KArgs;
  KArgs.Version = OMP_KERNEL_ARG_VERSION;
  KArgs.NumArgs = 4;
  KArgs.ArgBasePtrs = ArgBasePtrs;
  KArgs.ArgPtrs = ArgPtrs;
  KArgs.ArgSizes = ArgSizes;
  KArgs.ArgTypes = ArgTypes;
  KArgs.Flags.IsPtrArgs = 1;
  KArgs.Flags.StrictBlocksAndThreads = 1;
  KArgs.UserNumBlocks[0] = 1;
  KArgs.UserNumBlocks[1] = 1;
  KArgs.UserNumBlocks[2] = 1;
  KArgs.UserThreadLimit[0] = 1;
  KArgs.UserThreadLimit[1] = 1;
  KArgs.UserThreadLimit[2] = 1;

  if (__tgt_target_kernel(nullptr, Device, /*NumTeams=*/1, /*ThreadLimit=*/1,
                          OffloadEntries[0].Address, &KArgs) != 0) {
    std::fprintf(stderr, "error: __tgt_target_kernel failed\n");
    omp_target_free(DBuf, Device);
    omp_target_free(DRes, Device);
    return 1;
  }

  int B = 0;
  if (omp_target_memcpy(&B, DRes, sizeof(int), 0, 0, Host, Device) != 0) {
    std::fprintf(stderr, "error: omp_target_memcpy result from device failed\n");
    omp_target_free(DBuf, Device);
    omp_target_free(DRes, Device);
    return 1;
  }

  std::printf("b = %d\n", B);

  int BufferValue = 0;
  if (omp_target_memcpy(&BufferValue, DBuf, sizeof(int), 0, 0, Host, Device) !=
      0) {
    std::fprintf(stderr, "error: omp_target_memcpy from device failed\n");
    omp_target_free(DBuf, Device);
    omp_target_free(DRes, Device);
    return 1;
  }

  if (Mode == INPUTGEN_MODE_GENERATE) {
    std::printf("generated value = %d\n", BufferValue);
    FILE *F = std::fopen(InputFile, "w");
    if (!F) {
      std::fprintf(stderr, "error: failed to open %s for writing\n", InputFile);
      omp_target_free(DBuf, Device);
      omp_target_free(DRes, Device);
      return 1;
    }
    std::fprintf(F, "%d\n", BufferValue);
    std::fclose(F);
  } else {
    std::printf("replay value = %d\n", BufferValue);
  }

  omp_target_free(DBuf, Device);
  omp_target_free(DRes, Device);
  return 0;
}

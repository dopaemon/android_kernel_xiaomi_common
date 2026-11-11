# Build DoraCore GKI 6.6 bằng Docker

Build kernel trên máy cá nhân (macOS, Linux) bằng `docker compose up`, dựa trên
workflow `.github/workflows/GKI-6.6.yml`.

Docker **không tự clone kernel**. Bạn clone một lần, chỉ đường dẫn cho compose,
rồi build bao nhiêu lần cũng được.

## Các file liên quan

| File | Vai trò |
|---|---|
| `docker-compose.yml` | Toàn bộ settings và đường dẫn tới thư mục kernel |
| `docker/Dockerfile` | Image Ubuntu 24.04 với bộ package giống workflow |
| `docker/build.sh` | Script build, lấy từ step "Build Selected Variants" |
| `dist/` | Kết quả build (zip flash và `Image`), không đưa vào git |
| `.env` | Settings riêng của bạn (tự tạo, không đưa vào git) |

## Bắt đầu nhanh

1. Clone kernel, chỉ làm một lần. Trên macOS phải clone vào volume phân biệt
   hoa/thường, xem [Lưu ý cho macOS](#lưu-ý-cho-macos).

   ```bash
   git clone -b Rebase-6.6 --single-branch --recurse-submodules \
     https://github.com/dopaemon/android_kernel_xiaomi_common.git
   ```

2. Chỉ đường dẫn kernel cho compose bằng `KERNEL_DIR`
   (xem [Cách đổi settings](#cách-đổi-settings)). Mặc định là `../kernel`.

3. Build:

   ```bash
   docker compose up
   ```

4. Lấy kết quả trong `./dist`:

   - `DoraCore-<device>-<variant>-6.6-A15-<ngày giờ>.zip`: zip AnyKernel3 để flash
   - `Image-<variant>`: file `Image` thô

Lần chạy đầu sẽ lâu hơn vì phải build image Docker, tải AOSP clang và
AnyKernel3. Các lần sau dùng lại hết.

## Cách đổi settings

Có ba cách, ưu tiên từ trên xuống:

1. Truyền lúc chạy, chỉ áp dụng cho lần đó:

   ```bash
   KERNEL_DIR=/Volumes/kernel/android_kernel_xiaomi_common LTO_MODE=None docker compose up
   ```

2. Tạo file `.env` cạnh `docker-compose.yml`, áp dụng lâu dài:

   ```dotenv
   KERNEL_DIR=/Volumes/kernel/android_kernel_xiaomi_common
   BUILD_TARGET=KSU
   LTO_MODE=Thin
   ```

3. Sửa giá trị mặc định sau dấu `:-` trong `docker-compose.yml`.

## Danh sách settings

### Đường dẫn

| Setting | Mặc định | Ý nghĩa |
|---|---|---|
| `KERNEL_DIR` | `../kernel` | Thư mục kernel đã clone trên máy |
| `DIST_DIR` | `./dist` | Nơi nhận zip và `Image` |

### Build cái gì

| Setting | Mặc định | Giá trị | Ý nghĩa |
|---|---|---|---|
| `BUILD_TARGET` | `KSU` | `KSU`, `NonKSU`, `Both` | Bản có KernelSU, không có, hoặc cả hai trong một zip |
| `LTO_MODE` | `Thin` | `Thin`, `Full`, `None` | Chế độ LTO của clang. `None` build nhanh nhất |
| `DEVICE` | `gki` | tên máy bất kỳ | Chỉ dùng để đặt tên zip, config dùng chung |
| `MAKE_TARGET` | `Image` | target của `make` | Target cần build |
| `EXTRA_CONFIG` | trống | tham số `scripts/config` | Bật/tắt thêm config sau `gki_defconfig`, ví dụ `-d DEBUG_INFO_BTF -e SOME_OPTION` |
| `PACK_ZIP` | `1` | `0`, `1` | `0` = chỉ chép `Image` ra `dist`, không đóng zip |
| `ANYKERNEL_GIT` | repo Anykernel3 của dopaemon | URL git | Nguồn template AnyKernel3 |

### Toolchain

| Setting | Mặc định | Giá trị | Ý nghĩa |
|---|---|---|---|
| `TOOLCHAIN` | `aosp` | `aosp`, `system` | `aosp` = clang do kernel ghim trong `build.config.constants`, giống CI. `system` = clang 18 của Ubuntu 24.04 có sẵn trong image |
| `DOCKER_PLATFORM` | `linux/amd64` | `linux/amd64`, `linux/arm64` | Kiến trúc container. `aosp` bắt buộc `linux/amd64` |
| `CLANG_BRANCH` | `main-kernel-build-2024` | nhánh prebuilts AOSP | Nhánh chứa clang được ghim |
| `CLANG_FALLBACK_VERSION` | `r510928` | phiên bản clang | Dùng khi kernel không khai báo `CLANG_VERSION` |

### Tốc độ

| Setting | Mặc định | Ý nghĩa |
|---|---|---|
| `BUILD_JOBS` | trống (tất cả CPU cấp cho Docker) | Số job của `make` |
| `USE_CCACHE` | `1` | Dùng ccache |
| `CCACHE_SIZE` | `5G` | Dung lượng tối đa của ccache |
| `CLEAN_BUILD` | `0` | `1` = xoá thư mục out trước khi build, giống CI |

### Khác

| Setting | Mặc định | Ý nghĩa |
|---|---|---|
| `STRIP_PROTECTED_EXPORTS` | `1` | Build không có `android/abi_gki_protected_exports_*` như CI. File được khôi phục sau khi build |
| `TZ` | `Asia/Ho_Chi_Minh` | Múi giờ cho ngày giờ trong tên zip |

## Ví dụ

```bash
# Bản KSU, Thin LTO (mặc định)
docker compose up

# Build nhanh để thử, không LTO
LTO_MODE=None docker compose up

# Cả KSU và NonKSU trong một zip
BUILD_TARGET=Both docker compose up

# Build sạch từ đầu
CLEAN_BUILD=1 docker compose up

# Apple Silicon, build native cho nhanh
DOCKER_PLATFORM=linux/arm64 TOOLCHAIN=system docker compose up --build

# Chỉ lấy Image, không đóng zip
PACK_ZIP=0 docker compose up
```

## Lưu ý cho macOS

### Kernel phải nằm trên volume phân biệt hoa/thường

Ổ đĩa macOS mặc định không phân biệt chữ hoa/thường, trong khi cây Linux có
những file chỉ khác nhau chữ hoa/thường (ví dụ `xt_CONNMARK.h` và
`xt_connmark.h`). Clone lên ổ mặc định sẽ ra một checkout hỏng. Script build
kiểm tra điều này và dừng ngay kèm thông báo.

Tạo một volume riêng rồi clone vào đó:

```bash
hdiutil create -type SPARSEBUNDLE -fs 'Case-sensitive APFS' \
  -size 60g -volname kernel ~/kernel.sparsebundle
hdiutil attach ~/kernel.sparsebundle        # gắn tại /Volumes/kernel

cd /Volumes/kernel
git clone -b Rebase-6.6 --single-branch --recurse-submodules \
  https://github.com/dopaemon/android_kernel_xiaomi_common.git
```

Sau đó đặt `KERNEL_DIR=/Volumes/kernel/android_kernel_xiaomi_common`. Mỗi lần
khởi động lại máy cần chạy lại `hdiutil attach` trước khi build.

### Apple Silicon

AOSP clang chỉ có bản cho x86_64, nên mặc định container chạy `linux/amd64`
qua giả lập. Kết quả dùng đúng compiler của CI nhưng build chậm.

Muốn nhanh thì chạy container arm64 native với clang của Ubuntu:

```bash
DOCKER_PLATFORM=linux/arm64 TOOLCHAIN=system docker compose up --build
```

Cả hai đều là clang 18, nhưng bản `system` không phải đúng bản build của
Google, nên dùng bản `aosp` cho bản phát hành.

### Tài nguyên Docker Desktop

Chỉnh trong Settings > Resources. Con số dưới đây là ước lượng:

- RAM: tối thiểu 8 GB cho Thin LTO, 16 GB trở lên cho Full LTO
- Đĩa: khoảng 30 GB cho image, thư mục out, ccache và clang

## Dữ liệu nằm ở đâu

| Thứ | Vị trí | Ghi chú |
|---|---|---|
| Mã nguồn kernel | `KERNEL_DIR` trên máy | Gắn vào container tại `/work/common` |
| Zip và `Image` | `DIST_DIR` trên máy | |
| Thư mục out | volume Docker `out` | Tách riêng cho KSU và NonKSU, giữ lại giữa các lần build |
| AOSP clang, ccache, AnyKernel3 | volume Docker `cache` | Tải một lần, ccache tách theo toolchain |

Thư mục out và cache để trong volume của Docker vì nhanh hơn nhiều so với thư
mục gắn từ macOS, và phân biệt hoa/thường.

## Lệnh hay dùng

```bash
docker compose up              # build
docker compose up --build      # build lại image sau khi sửa Dockerfile
docker compose down            # xoá container, giữ cache
docker compose down -v         # xoá luôn thư mục out và toàn bộ cache
```

Sửa `docker/build.sh` thì không cần build lại image, vì script được gắn thẳng
vào container.

## Khác gì so với workflow trên GitHub

- **Không clone kernel**, và không `git reset --hard`. Thay đổi chưa commit
  trong cây kernel của bạn được giữ nguyên và được build luôn.
- **Protected exports**: workflow xoá hẳn
  `android/abi_gki_protected_exports_*` khỏi bản clone tạm. Ở đây file bị xoá
  tạm trong lúc build rồi khôi phục khi build xong, lỗi hoặc Ctrl-C. Vì vậy
  tên kernel có đuôi `-dirty`, giống bản CI.
- **Build tăng dần**: thư mục out được giữ lại. Đặt `CLEAN_BUILD=1` để build
  sạch như CI.
- **Số job**: dùng tất cả CPU thay vì giới hạn 4 như runner GitHub.
- **`CONFIG_UAPI_HEADER_TEST` luôn tắt**: option này tự bật khi compiler link
  được với libc arm64 (container arm64 native) và làm build lỗi ở
  `usr/include/media/ispv3_defs.h`. Trên CI nó vốn đã tắt, và nó không ảnh
  hưởng tới `Image`.
- **ccache**: kiểm tra compiler theo chuỗi `--version` thay vì bỏ qua, để hai
  toolchain không dùng nhầm object của nhau.
- **Không có** phần tạo release, upload, gửi Telegram, chỉnh swap hay dọn đĩa.

## Xử lý sự cố

| Hiện tượng | Nguyên nhân và cách xử lý |
|---|---|
| `no kernel source at /work/common` | `KERNEL_DIR` sai hoặc chưa clone kernel |
| `the kernel was cloned on a case-insensitive filesystem` | Clone lại kernel trên volume phân biệt hoa/thường |
| `TOOLCHAIN=aosp needs an x86_64 container` | Đang chạy `linux/arm64` với `aosp`. Đổi về `linux/amd64` hoặc dùng `TOOLCHAIN=system` |
| Build bị kill giữa chừng, thường ở bước link | Thiếu RAM. Tăng RAM cho Docker Desktop, giảm `BUILD_JOBS`, hoặc dùng `LTO_MODE=None` |
| `No space left on device` | Tăng dung lượng đĩa của Docker Desktop hoặc chạy `docker compose down -v` |
| `source tree is not clean` | Trong cây kernel có file từ lần build tại chỗ. Chạy `make mrproper` trong thư mục kernel |
| Thiếu `android/abi_gki_protected_exports_*` sau khi container bị kill cứng | Chạy `git checkout -- android/` trong thư mục kernel |
| Đổi `DOCKER_PLATFORM` mà vẫn chạy image cũ | Chạy `docker compose up --build` |

## Trạng thái kiểm thử

- `docker compose config` hợp lệ.
- `docker/build.sh` đã chạy trực tiếp trên host Linux x86_64 với AOSP clang
  r510928, `LTO_MODE=None`, bản KSU: ra `Image` và zip, kernel
  `6.6.144-DoraCore`.
- Chưa chạy qua Docker thật và chưa thử trên macOS. Chế độ `TOOLCHAIN=system`
  và `BUILD_TARGET=Both` cũng chưa chạy thử.

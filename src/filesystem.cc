// Copyright 2019-2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "filesystem.h"

#ifdef _WIN32
// suppress the min and max definitions in Windef.h.
#define NOMINMAX
#include <Windows.h>

// _CRT_INTERNAL_NONSTDC_NAMES 1 before including Microsoft provided C Runtime
// library to expose declarations without "_" prefix to match POSIX style.
#define _CRT_INTERNAL_NONSTDC_NAMES 1
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#ifdef TRITON_ENABLE_GCS
#include <google/cloud/storage/client.h>
#endif  // TRITON_ENABLE_GCS

#ifdef TRITON_ENABLE_S3
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#endif  // TRITON_ENABLE_S3

#ifdef TRITON_ENABLE_AZURE_STORAGE
#include <blob/blob_client.h>
#include <storage_account.h>
#include <storage_credential.h>
#undef LOG_INFO
#undef LOG_WARNING
#endif  // TRITON_ENABLE_AZURE_STORAGE

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/text_format.h>
#include <re2/re2.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <fstream>
#include <mutex>

#include "constants.h"
#include "status.h"
#include "triton/common/logging.h"

#define TRITONJSON_STATUSTYPE triton::core::Status
#define TRITONJSON_STATUSRETURN(M) \
  return triton::core::Status(triton::core::Status::Code::INTERNAL, (M))
#define TRITONJSON_STATUSSUCCESS triton::core::Status::Success
#include "triton/common/triton_json.h"

#ifdef _WIN32
// <sys/stat.h> in Windows doesn't define S_ISDIR macro
#if !defined(S_ISDIR) && defined(S_IFMT) && defined(S_IFDIR)
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#endif
#define F_OK 0
#endif

namespace triton { namespace core {

namespace {

// Check if a local path is a directory. We need to use this in
// LocalFileSystem and LocalizedDirectory so have this common
// function.
Status
IsPathDirectory(const std::string& path, bool* is_dir)
{
  *is_dir = false;

  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return Status(Status::Code::INTERNAL, "failed to stat file " + path);
  }

  *is_dir = S_ISDIR(st.st_mode);
  return Status::Success;
}

}  // namespace

LocalizedDirectory::~LocalizedDirectory()
{
  if (!local_path_.empty()) {
    LOG_STATUS_ERROR(
        DeleteDirectory(local_path_),
        "failed to delete localized model directory");
  }
}

namespace {

class FileSystem {
 public:
  virtual Status FileExists(const std::string& path, bool* exists) = 0;
  virtual Status IsDirectory(const std::string& path, bool* is_dir) = 0;
  virtual Status FileModificationTime(
      const std::string& path, int64_t* mtime_ns) = 0;
  virtual Status GetDirectoryContents(
      const std::string& path, std::set<std::string>* contents) = 0;
  virtual Status GetDirectorySubdirs(
      const std::string& path, std::set<std::string>* subdirs) = 0;
  virtual Status GetDirectoryFiles(
      const std::string& path, std::set<std::string>* files) = 0;
  virtual Status ReadTextFile(
      const std::string& path, std::string* contents) = 0;
  virtual Status LocalizeDirectory(
      const std::string& path,
      std::shared_ptr<LocalizedDirectory>* localized) = 0;
  virtual Status WriteTextFile(
      const std::string& path, const std::string& contents) = 0;
  virtual Status WriteBinaryFile(
      const std::string& path, const char* contents,
      const size_t content_len) = 0;
  virtual Status MakeDirectory(
      const std::string& dir, const bool recursive) = 0;
  virtual Status MakeTemporaryDirectory(std::string* temp_dir) = 0;
  virtual Status DeleteDirectory(const std::string& path) = 0;
};

class LocalFileSystem final : public FileSystem {
 public:
  Status FileExists(const std::string& path, bool* exists) override;
  Status IsDirectory(const std::string& path, bool* is_dir) override;
  Status FileModificationTime(
      const std::string& path, int64_t* mtime_ns) override;
  Status GetDirectoryContents(
      const std::string& path, std::set<std::string>* contents) override;
  Status GetDirectorySubdirs(
      const std::string& path, std::set<std::string>* subdirs) override;
  Status GetDirectoryFiles(
      const std::string& path, std::set<std::string>* files) override;
  Status ReadTextFile(const std::string& path, std::string* contents) override;
  Status LocalizeDirectory(
      const std::string& path,
      std::shared_ptr<LocalizedDirectory>* localized) override;
  Status WriteTextFile(
      const std::string& path, const std::string& contents) override;
  Status WriteBinaryFile(
      const std::string& path, const char* contents,
      const size_t content_len) override;
  Status MakeDirectory(const std::string& dir, const bool recursive) override;
  Status MakeTemporaryDirectory(std::string* temp_dir) override;
  Status DeleteDirectory(const std::string& path) override;
};

Status
LocalFileSystem::FileExists(const std::string& path, bool* exists)
{
  *exists = (access(path.c_str(), F_OK) == 0);
  return Status::Success;
}

Status
LocalFileSystem::IsDirectory(const std::string& path, bool* is_dir)
{
  return IsPathDirectory(path, is_dir);
}

Status
LocalFileSystem::FileModificationTime(
    const std::string& path, int64_t* mtime_ns)
{
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return Status(Status::Code::INTERNAL, "failed to stat file " + path);
  }

#ifdef _WIN32
  // In Windows, st_mtime is in time_t
  *mtime_ns = st.st_mtime;
#else
  *mtime_ns = TIMESPEC_TO_NANOS(st.st_mtim);
#endif
  return Status::Success;
}

Status
LocalFileSystem::GetDirectoryContents(
    const std::string& path, std::set<std::string>* contents)
{
#ifdef _WIN32
  WIN32_FIND_DATA entry;
  // Append "*" to obtain all files under 'path'
  HANDLE dir = FindFirstFile(JoinPath({path, "*"}).c_str(), &entry);
  if (dir == INVALID_HANDLE_VALUE) {
    return Status(Status::Code::INTERNAL, "failed to open directory " + path);
  }
  if ((strcmp(entry.cFileName, ".") != 0) &&
      (strcmp(entry.cFileName, "..") != 0)) {
    contents->insert(entry.cFileName);
  }
  while (FindNextFile(dir, &entry)) {
    if ((strcmp(entry.cFileName, ".") != 0) &&
        (strcmp(entry.cFileName, "..") != 0)) {
      contents->insert(entry.cFileName);
    }
  }

  FindClose(dir);
#else
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    return Status(Status::Code::INTERNAL, "failed to open directory " + path);
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string entryname = entry->d_name;
    if ((entryname != ".") && (entryname != "..")) {
      contents->insert(entryname);
    }
  }

  closedir(dir);
#endif
  return Status::Success;
}

Status
LocalFileSystem::GetDirectorySubdirs(
    const std::string& path, std::set<std::string>* subdirs)
{
  RETURN_IF_ERROR(GetDirectoryContents(path, subdirs));

  // Erase non-directory entries...
  for (auto iter = subdirs->begin(); iter != subdirs->end();) {
    bool is_dir;
    RETURN_IF_ERROR(IsDirectory(JoinPath({path, *iter}), &is_dir));
    if (!is_dir) {
      iter = subdirs->erase(iter);
    } else {
      ++iter;
    }
  }

  return Status::Success;
}

Status
LocalFileSystem::GetDirectoryFiles(
    const std::string& path, std::set<std::string>* files)
{
  RETURN_IF_ERROR(GetDirectoryContents(path, files));

  // Erase directory entries...
  for (auto iter = files->begin(); iter != files->end();) {
    bool is_dir;
    RETURN_IF_ERROR(IsDirectory(JoinPath({path, *iter}), &is_dir));
    if (is_dir) {
      iter = files->erase(iter);
    } else {
      ++iter;
    }
  }

  return Status::Success;
}

Status
LocalFileSystem::ReadTextFile(const std::string& path, std::string* contents)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) {
    return Status(
        Status::Code::INTERNAL,
        "failed to open text file for read " + path + ": " + strerror(errno));
  }

  in.seekg(0, std::ios::end);
  contents->resize(in.tellg());
  in.seekg(0, std::ios::beg);
  in.read(&(*contents)[0], contents->size());
  in.close();

  return Status::Success;
}

Status
LocalFileSystem::LocalizeDirectory(
    const std::string& path, std::shared_ptr<LocalizedDirectory>* localized)
{
  // For local file system we don't actually need to download the
  // directory. We use it in place.
  localized->reset(new LocalizedDirectory(path));
  return Status::Success;
}

Status
LocalFileSystem::WriteTextFile(
    const std::string& path, const std::string& contents)
{
  std::ofstream out(path, std::ios::out | std::ios::binary);
  if (!out) {
    return Status(
        Status::Code::INTERNAL,
        "failed to open text file for write " + path + ": " + strerror(errno));
  }

  out.write(&contents[0], contents.size());
  out.close();

  return Status::Success;
}

Status
LocalFileSystem::WriteBinaryFile(
    const std::string& path, const char* contents, const size_t content_len)
{
  std::ofstream out(path, std::ios::out | std::ios::binary);
  if (!out) {
    return Status(
        Status::Code::INTERNAL, "failed to open binary file for write " + path +
                                    ": " + strerror(errno));
  }

  out.write(contents, content_len);

  return Status::Success;
}

Status
LocalFileSystem::MakeDirectory(const std::string& dir, const bool recursive)
{
#ifdef _WIN32
  if (mkdir(dir.c_str()) == -1)
#else
  if (mkdir(dir.c_str(), S_IRWXU) == -1)
#endif
  {
    // Only allow the error due to parent directory does not exist
    // if 'recursive' is requested
    if ((errno == ENOENT) && (!dir.empty()) && recursive) {
      RETURN_IF_ERROR(MakeDirectory(DirName(dir), recursive));
      // Retry the creation
#ifdef _WIN32
      if (mkdir(dir.c_str()) == -1)
#else
      if (mkdir(dir.c_str(), S_IRWXU) == -1)
#endif
      {
        return Status(
            Status::Code::INTERNAL, "Failed to create directory '" + dir +
                                        "', errno:" + strerror(errno));
      }
    } else {
      return Status(
          Status::Code::INTERNAL,
          "Failed to create directory '" + dir + "', errno:" + strerror(errno));
    }
  }

  return Status::Success;
}

Status
LocalFileSystem::MakeTemporaryDirectory(std::string* temp_dir)
{
#ifdef _WIN32
  char temp_path[MAX_PATH + 1];
  size_t temp_path_length = GetTempPath(MAX_PATH + 1, temp_path);
  if (temp_path_length == 0) {
    return Status(
        Status::Code::INTERNAL,
        "Failed to get local directory for temporary files");
  }
  // There is no single operation like 'mkdtemp' in Windows, thus generating
  // unique temporary directory is a process of getting temporary file name,
  // deleting the file (file creation is side effect fo getting name), creating
  // corresponding directory, so mutex is used to avoid possible race condition.
  // However, it doesn't prevent other process on creating temporary file and
  // thus the race condition may still happen. One possible solution is
  // to reserve a temporary directory for the process and generate temporary
  // model directories inside it.
  static std::mutex mtx;
  std::lock_guard<std::mutex> lk(mtx);
  // Construct a std::string as filled 'temp_path' is not C string,
  // and so that we can reuse 'temp_path' to hold the temp file name.
  std::string temp_path_str(temp_path, temp_path_length);
  if (GetTempFileName(temp_path_str.c_str(), "folder", 0, temp_path) == 0) {
    return Status(Status::Code::INTERNAL, "Failed to create local temp folder");
  }
  *temp_dir = temp_path;
  DeleteFile(temp_dir->c_str());
  if (CreateDirectory(temp_dir->c_str(), NULL) == 0) {
    return Status(
        Status::Code::INTERNAL,
        "Failed to create local temp folder: " + *temp_dir);
  }
#else
  std::string folder_template = "/tmp/folderXXXXXX";
  char* res = mkdtemp(const_cast<char*>(folder_template.c_str()));
  if (res == nullptr) {
    return Status(
        Status::Code::INTERNAL,
        "Failed to create local temp folder: " + folder_template +
            ", errno:" + strerror(errno));
  }
  *temp_dir = res;
#endif
  return Status::Success;
}

Status
LocalFileSystem::DeleteDirectory(const std::string& path)
{
  std::set<std::string> contents;
  RETURN_IF_ERROR(GetDirectoryContents(path, &contents));

  for (const auto& content : contents) {
    std::string full_path = JoinPath({path, content});
    bool is_dir = false;
    RETURN_IF_ERROR(IsDirectory(full_path, &is_dir));
    if (is_dir) {
      DeleteDirectory(full_path);
    } else {
      remove(full_path.c_str());
    }
  }
  rmdir(path.c_str());

  return Status::Success;
}

#if defined(TRITON_ENABLE_GCS) || defined(TRITON_ENABLE_S3) || \
    defined(TRITON_ENABLE_AZURE_STORAGE)
// Helper function to take care of lack of trailing slashes
std::string
AppendSlash(const std::string& name)
{
  if (name.empty() || (name.back() == '/')) {
    return name;
  }

  return (name + "/");
}
#endif  // TRITON_ENABLE_GCS || TRITON_ENABLE_S3 || TRITON_ENABLE_AZURE_STORAGE

#ifdef TRITON_ENABLE_GCS

namespace gcs = google::cloud::storage;

class GCSFileSystem final : public FileSystem {
 public:
  GCSFileSystem();
  GCSFileSystem(const std::string& credential_path);
  Status CheckClient();
  Status FileExists(const std::string& path, bool* exists) override;
  Status IsDirectory(const std::string& path, bool* is_dir) override;
  Status FileModificationTime(
      const std::string& path, int64_t* mtime_ns) override;
  Status GetDirectoryContents(
      const std::string& path, std::set<std::string>* contents) override;
  Status GetDirectorySubdirs(
      const std::string& path, std::set<std::string>* subdirs) override;
  Status GetDirectoryFiles(
      const std::string& path, std::set<std::string>* files) override;
  Status ReadTextFile(const std::string& path, std::string* contents) override;
  Status LocalizeDirectory(
      const std::string& path,
      std::shared_ptr<LocalizedDirectory>* localized) override;
  Status WriteTextFile(
      const std::string& path, const std::string& contents) override;
  Status WriteBinaryFile(
      const std::string& path, const char* contents,
      const size_t content_len) override;
  Status MakeDirectory(const std::string& dir, const bool recursive) override;
  Status MakeTemporaryDirectory(std::string* temp_dir) override;
  Status DeleteDirectory(const std::string& path) override;

 private:
  Status ParsePath(
      const std::string& path, std::string* bucket, std::string* object);
  Status MetaDataExists(
      const std::string path, bool* exists,
      google::cloud::StatusOr<gcs::ObjectMetadata>* metadata);

  google::cloud::StatusOr<gcs::Client> client_;
  static std::mutex env_mu_;  // protect GOOGLE_APPLICATION_CREDENTIALS env var
};
std::mutex GCSFileSystem::env_mu_;

GCSFileSystem::GCSFileSystem()
{
  std::lock_guard<std::mutex> lock(env_mu_);
  client_ = gcs::Client::CreateDefaultClient();
}

GCSFileSystem::GCSFileSystem(const std::string& credential_path)
{
  std::string env_name = "GOOGLE_APPLICATION_CREDENTIALS";
  std::lock_guard<std::mutex> lock(env_mu_);
#ifdef _WIN32
  _putenv((env_name + "=" + credential_path).c_str());
#else
  setenv(env_name.c_str(), credential_path.c_str(), 1);
#endif
  client_ = gcs::Client::CreateDefaultClient();
}

Status
GCSFileSystem::CheckClient()
{
  // Need to return error status if GOOGLE_APPLICATION_CREDENTIALS is not set or
  // valid
  if (!client_) {
    return Status(
        Status::Code::INTERNAL,
        "Unable to create GCS client. Check account credentials.");
  }
  return Status::Success;
}

Status
GCSFileSystem::ParsePath(
    const std::string& path, std::string* bucket, std::string* object)
{
  // Get the bucket name and the object path. Return error if input is malformed
  int bucket_start = path.find("gs://") + strlen("gs://");
  int bucket_end = path.find("/", bucket_start);

  // If there isn't a second slash, the address has only the bucket
  if (bucket_end > bucket_start) {
    *bucket = path.substr(bucket_start, bucket_end - bucket_start);
    *object = path.substr(bucket_end + 1);
  } else {
    *bucket = path.substr(bucket_start);
    *object = "";
  }

  if (bucket->empty()) {
    return Status(
        Status::Code::INTERNAL, "No bucket name found in path: " + path);
  }

  return Status::Success;
}

Status
GCSFileSystem::FileExists(const std::string& path, bool* exists)
{
  *exists = false;

  std::string bucket, object;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object));

  // Make a request for metadata and check the response
  google::cloud::StatusOr<gcs::ObjectMetadata> object_metadata =
      client_->GetObjectMetadata(bucket, object);

  if (object_metadata) {
    *exists = true;
    return Status::Success;
  }

  // GCS doesn't make objects for directories, so it could still be a directory
  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  *exists = is_dir;

  return Status::Success;
}

Status
GCSFileSystem::IsDirectory(const std::string& path, bool* is_dir)
{
  *is_dir = false;
  std::string bucket, object_path;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object_path));

  // Check if the bucket exists
  google::cloud::StatusOr<gcs::BucketMetadata> bucket_metadata =
      client_->GetBucketMetadata(bucket);

  if (!bucket_metadata) {
    return Status(
        Status::Code::INTERNAL, "Could not get MetaData for bucket with name " +
                                    bucket + " : " +
                                    bucket_metadata.status().message());
  }

  // Root case - bucket exists and object path is empty
  if (object_path.empty()) {
    *is_dir = true;
    return Status::Success;
  }

  // Check whether it has children. If at least one child, it is a directory
  for (auto&& object_metadata :
       client_->ListObjects(bucket, gcs::Prefix(AppendSlash(object_path)))) {
    if (object_metadata) {
      *is_dir = true;
      break;
    }
  }
  return Status::Success;
}

Status
GCSFileSystem::FileModificationTime(const std::string& path, int64_t* mtime_ns)
{
  // We don't need to worry about the case when this is a directory
  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  if (is_dir) {
    *mtime_ns = 0;
    return Status::Success;
  }

  std::string bucket, object;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object));

  // Otherwise check the object metadata for update time
  google::cloud::StatusOr<gcs::ObjectMetadata> object_metadata =
      client_->GetObjectMetadata(bucket, object);

  if (!object_metadata) {
    return Status(
        Status::Code::INTERNAL, "Failed to get metadata for " + object + " : " +
                                    object_metadata.status().message());
  }

  // Get duration from time point with respect to object clock
  auto update_time = std::chrono::time_point_cast<std::chrono::nanoseconds>(
                         object_metadata->updated())
                         .time_since_epoch()
                         .count();

  *mtime_ns = update_time;
  return Status::Success;
}

Status
GCSFileSystem::GetDirectoryContents(
    const std::string& path, std::set<std::string>* contents)
{
  std::string bucket, dir_path;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &dir_path));
  // Append a slash to make it easier to list contents
  std::string full_dir = AppendSlash(dir_path);

  // Get objects with prefix equal to full directory path
  for (auto&& object_metadata :
       client_->ListObjects(bucket, gcs::Prefix(full_dir))) {
    if (!object_metadata) {
      return Status(
          Status::Code::INTERNAL, "Could not list contents of directory at " +
                                      path + " : " +
                                      object_metadata.status().message());
    }

    // In the case of empty directories, the directory itself will appear here
    if (object_metadata->name() == full_dir) {
      continue;
    }

    // We have to make sure that subdirectory contents do not appear here
    std::string name = object_metadata->name();
    int item_start = name.find(full_dir) + full_dir.size();
    // GCS response prepends parent directory name
    int item_end = name.find("/", item_start);

    // Let set take care of subdirectory contents
    std::string item = name.substr(item_start, item_end - item_start);
    contents->insert(item);
  }
  return Status::Success;
}

Status
GCSFileSystem::GetDirectorySubdirs(
    const std::string& path, std::set<std::string>* subdirs)
{
  RETURN_IF_ERROR(GetDirectoryContents(path, subdirs));

  // Erase non-directory entries...
  for (auto iter = subdirs->begin(); iter != subdirs->end();) {
    bool is_dir;
    RETURN_IF_ERROR(IsDirectory(JoinPath({path, *iter}), &is_dir));
    if (!is_dir) {
      iter = subdirs->erase(iter);
    } else {
      ++iter;
    }
  }

  return Status::Success;
}

Status
GCSFileSystem::GetDirectoryFiles(
    const std::string& path, std::set<std::string>* files)
{
  RETURN_IF_ERROR(GetDirectoryContents(path, files));

  // Erase directory entries...
  for (auto iter = files->begin(); iter != files->end();) {
    bool is_dir;
    RETURN_IF_ERROR(IsDirectory(JoinPath({path, *iter}), &is_dir));
    if (is_dir) {
      iter = files->erase(iter);
    } else {
      ++iter;
    }
  }

  return Status::Success;
}

Status
GCSFileSystem::ReadTextFile(const std::string& path, std::string* contents)
{
  bool exists;
  RETURN_IF_ERROR(FileExists(path, &exists));

  if (!exists) {
    return Status(Status::Code::INTERNAL, "File does not exist at " + path);
  }

  std::string bucket, object;
  ParsePath(path, &bucket, &object);

  gcs::ObjectReadStream stream = client_->ReadObject(bucket, object);

  if (!stream) {
    return Status(
        Status::Code::INTERNAL, "Failed to open object read stream for " +
                                    path + " : " + stream.status().message());
  }

  std::string data = "";
  char c;
  while (stream.get(c)) {
    data += c;
  }

  *contents = data;

  return Status::Success;
}

Status
GCSFileSystem::LocalizeDirectory(
    const std::string& path, std::shared_ptr<LocalizedDirectory>* localized)
{
  bool exists;
  RETURN_IF_ERROR(FileExists(path, &exists));

  bool is_dir = false;
  if (exists) {
    RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  }

  if (!is_dir) {
    return Status(
        Status::Code::INTERNAL, "directory does not exist at " + path);
  }

  std::string tmp_folder;
  RETURN_IF_ERROR(
      triton::core::MakeTemporaryDirectory(FileSystemType::LOCAL, &tmp_folder));

  localized->reset(new LocalizedDirectory(path, tmp_folder));

  std::set<std::string> contents, filenames;
  RETURN_IF_ERROR(GetDirectoryContents(path, &filenames));
  for (auto itr = filenames.begin(); itr != filenames.end(); ++itr) {
    contents.insert(JoinPath({path, *itr}));
  }

  while (contents.size() != 0) {
    std::set<std::string> tmp_contents = contents;
    contents.clear();
    for (auto iter = tmp_contents.begin(); iter != tmp_contents.end(); ++iter) {
      bool is_subdir;
      std::string gcs_fpath = *iter;
      std::string gcs_removed_path = gcs_fpath.substr(path.size());
      std::string local_fpath =
          JoinPath({(*localized)->Path(), gcs_removed_path});
      RETURN_IF_ERROR(IsDirectory(gcs_fpath, &is_subdir));
      if (is_subdir) {
        // Create local mirror of sub-directories
#ifdef _WIN32
        int status = mkdir(const_cast<char*>(local_fpath.c_str()));
#else
        int status = mkdir(
            const_cast<char*>(local_fpath.c_str()),
            S_IRUSR | S_IWUSR | S_IXUSR);
#endif
        if (status == -1) {
          return Status(
              Status::Code::INTERNAL,
              "Failed to create local folder: " + local_fpath +
                  ", errno:" + strerror(errno));
        }

        // Add sub-directories and deeper files to contents
        std::set<std::string> subdir_contents;
        RETURN_IF_ERROR(GetDirectoryContents(gcs_fpath, &subdir_contents));
        for (auto itr = subdir_contents.begin(); itr != subdir_contents.end();
             ++itr) {
          contents.insert(JoinPath({gcs_fpath, *itr}));
        }
      } else {
        // Create local copy of file
        std::string file_bucket, file_object;
        RETURN_IF_ERROR(ParsePath(gcs_fpath, &file_bucket, &file_object));

        // Send a request to read the object
        gcs::ObjectReadStream filestream =
            client_->ReadObject(file_bucket, file_object);
        if (!filestream) {
          return Status(
              Status::Code::INTERNAL, "Failed to get object at " + *iter +
                                          " : " +
                                          filestream.status().message());
        }

        std::string gcs_removed_path = (*iter).substr(path.size());
        std::string local_file_path =
            JoinPath({(*localized)->Path(), gcs_removed_path});
        std::ofstream output_file(local_file_path.c_str(), std::ios::binary);
        output_file << filestream.rdbuf();
        output_file.close();
      }
    }
  }

  return Status::Success;
}

Status
GCSFileSystem::WriteTextFile(
    const std::string& path, const std::string& contents)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Write text file operation not yet implemented " + path);
}

Status
GCSFileSystem::WriteBinaryFile(
    const std::string& path, const char* contents, const size_t content_len)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Write text file operation not yet implemented " + path);
}

Status
GCSFileSystem::MakeDirectory(const std::string& dir, const bool recursive)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Make temporary directory operation not yet implemented");
}

Status
GCSFileSystem::MakeTemporaryDirectory(std::string* temp_dir)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Make temporary directory operation not yet implemented");
}

Status
GCSFileSystem::DeleteDirectory(const std::string& path)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Delete directory operation not yet implemented");
}

#endif  // TRITON_ENABLE_GCS


#ifdef TRITON_ENABLE_AZURE_STORAGE

namespace as = azure::storage_lite;
const std::string AS_URL_PATTERN = "as://([^/]+)/([^/?]+)(?:/([^?]*))?(\\?.*)?";

class ASFileSystem final : public FileSystem {
 public:
  ASFileSystem(const std::string& s3_path);
  ASFileSystem(
      const std::string& s3_path, const char* account_str,
      const char* account_key);
  Status CheckClient();

  Status FileExists(const std::string& path, bool* exists) override;
  Status IsDirectory(const std::string& path, bool* is_dir) override;
  Status FileModificationTime(
      const std::string& path, int64_t* mtime_ns) override;
  Status GetDirectoryContents(
      const std::string& path, std::set<std::string>* contents) override;
  Status GetDirectorySubdirs(
      const std::string& path, std::set<std::string>* subdirs) override;
  Status GetDirectoryFiles(
      const std::string& path, std::set<std::string>* files) override;
  Status ReadTextFile(const std::string& path, std::string* contents) override;
  Status LocalizeDirectory(
      const std::string& path,
      std::shared_ptr<LocalizedDirectory>* localized) override;
  Status WriteTextFile(
      const std::string& path, const std::string& contents) override;
  Status WriteBinaryFile(
      const std::string& path, const char* contents,
      const size_t content_len) override;
  Status MakeDirectory(const std::string& dir, const bool recursive) override;
  Status MakeTemporaryDirectory(std::string* temp_dir) override;
  Status DeleteDirectory(const std::string& path) override;

 private:
  Status ParsePath(
      const std::string& path, std::string* bucket, std::string* object);
  std::shared_ptr<as::blob_client> client_;

  Status ListDirectory(
      const std::string& path, const std::string& dir_path,
      std::function<
          Status(const as::list_blobs_segmented_item&, const std::string&)>
          func);

  Status DownloadFolder(
      const std::string& container, const std::string& path,
      const std::string& dest);
  re2::RE2 as_regex_;
};

Status
ASFileSystem::ParsePath(
    const std::string& path, std::string* container, std::string* object)
{
  std::string host_name, query;
  if (!RE2::FullMatch(path, as_regex_, &host_name, container, object, &query)) {
    return Status(
        Status::Code::INTERNAL, "Invalid azure storage path: " + path);
  }
  return Status::Success;
}

ASFileSystem::ASFileSystem(const std::string& path)
    : ASFileSystem(
          path, std::getenv("AZURE_STORAGE_ACCOUNT"),
          std::getenv("AZURE_STORAGE_KEY"))
{
}

ASFileSystem::ASFileSystem(
    const std::string& path, const char* account_str, const char* account_key)
    : as_regex_(AS_URL_PATTERN)
{
  std::shared_ptr<as::storage_account> account = nullptr;
  std::string host_name, container, blob_path, query;
  if (RE2::FullMatch(
          path, as_regex_, &host_name, &container, &blob_path, &query)) {
    size_t pos = host_name.rfind(".blob.core.windows.net");
    std::string account_name;
    if (account_str == NULL) {
      if (pos != std::string::npos) {
        account_name = host_name.substr(0, pos);
      } else {
        account_name = host_name;
      }
    } else {
      account_name = std::string(account_str);
    }

    std::shared_ptr<as::storage_credential> cred;
    if (account_key != NULL) {
      // Shared Key
      cred = std::make_shared<as::shared_key_credential>(
          account_name, account_key);
    } else {
      cred = std::make_shared<as::anonymous_credential>();
    }
    account = std::make_shared<as::storage_account>(
        account_name, cred, /* use_https */ true);
    client_ =
        std::make_shared<as::blob_client>(account, /*max_concurrency*/ 16);
  }
}

Status
ASFileSystem::CheckClient()
{
  if (client_ == nullptr) {
    return Status(
        Status::Code::INTERNAL,
        "Unable to create Azure filesystem client. Check account credentials.");
  }
  return Status::Success;
}


Status
ASFileSystem::FileModificationTime(const std::string& path, int64_t* mtime_ns)
{
  as::blob_client_wrapper bc(client_);
  std::string container, object_path;
  RETURN_IF_ERROR(ParsePath(path, &container, &object_path));

  auto blobProperty = bc.get_blob_property(container, object_path);
  if (errno != 0) {
    return Status(
        Status::Code::INTERNAL, "Unable to get blob property for file at " +
                                    path + ", errno:" + strerror(errno));
  }

  auto time =
      std::chrono::system_clock::from_time_t(blobProperty.last_modified);
  auto update_time =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(time)
          .time_since_epoch()
          .count();

  *mtime_ns = update_time;
  return Status::Success;
};

Status
ASFileSystem::ListDirectory(
    const std::string& container, const std::string& dir_path,
    std::function<
        Status(const as::list_blobs_segmented_item&, const std::string&)>
        func)
{
  as::blob_client_wrapper bc(client_);

  // Append a slash to make it easier to list contents
  std::string full_dir = AppendSlash(dir_path);
  auto blobs = bc.list_blobs_segmented(container, "/", "", full_dir);
  if (errno != 0) {
    return Status(
        Status::Code::INTERNAL, "Failed to get contents of directory " +
                                    dir_path + ", errno:" + strerror(errno));
  }

  for (auto&& item : blobs.blobs) {
    std::string name = item.name;
    int item_start = name.find(full_dir) + full_dir.size();
    int item_end = name.find("/", item_start);
    // Let set take care of subdirectory contents
    std::string subfile = name.substr(item_start, item_end - item_start);
    auto status = func(item, subfile);
    if (!status.IsOk()) {
      return status;
    }
  }
  return Status::Success;
}

Status
ASFileSystem::GetDirectoryContents(
    const std::string& path, std::set<std::string>* contents)
{
  auto func = [&](const as::list_blobs_segmented_item& item,
                  const std::string& dir) {
    contents->insert(dir);
    return Status::Success;
  };
  std::string container, dir_path;
  RETURN_IF_ERROR(ParsePath(path, &container, &dir_path));
  return ListDirectory(container, dir_path, func);
}

Status
ASFileSystem::GetDirectorySubdirs(
    const std::string& path, std::set<std::string>* subdirs)
{
  auto func = [&](const as::list_blobs_segmented_item& item,
                  const std::string& dir) {
    if (item.is_directory) {
      subdirs->insert(dir);
    }
    return Status::Success;
  };
  std::string container, dir_path;
  RETURN_IF_ERROR(ParsePath(path, &container, &dir_path));
  return ListDirectory(container, dir_path, func);
}

Status
ASFileSystem::GetDirectoryFiles(
    const std::string& path, std::set<std::string>* files)
{
  auto func = [&](const as::list_blobs_segmented_item& item,
                  const std::string& file) {
    if (!item.is_directory) {
      files->insert(file);
    }
    return Status::Success;
  };
  std::string container, dir_path;
  RETURN_IF_ERROR(ParsePath(path, &container, &dir_path));
  return ListDirectory(container, dir_path, func);
}

Status
ASFileSystem::IsDirectory(const std::string& path, bool* is_dir)
{
  *is_dir = false;
  std::string container, object_path;
  RETURN_IF_ERROR(ParsePath(path, &container, &object_path));

  as::blob_client_wrapper bc(client_);
  auto blobs = bc.list_blobs_segmented(container, "/", "", object_path, 1);
  if (errno != 0) {
    return Status(
        Status::Code::INTERNAL, "Failed to check if directory at " + path +
                                    ", errno:" + strerror(errno));
  }
  *is_dir = blobs.blobs.size() > 0;

  return Status::Success;
};

Status
ASFileSystem::ReadTextFile(const std::string& path, std::string* contents)
{
  as::blob_client_wrapper bc(client_);
  std::string container, object_path;
  RETURN_IF_ERROR(ParsePath(path, &container, &object_path));
  using namespace azure::storage_lite;
  std::ostringstream out_stream;
  bc.download_blob_to_stream(container, object_path, 0, 0, out_stream);
  if (errno != 0) {
    return Status(
        Status::Code::INTERNAL, "Failed to fetch file stream at " + path +
                                    ", errno:" + strerror(errno));
  }
  *contents = out_stream.str();

  return Status::Success;
}

Status
ASFileSystem::FileExists(const std::string& path, bool* exists)
{
  *exists = false;

  std::string container, object;
  RETURN_IF_ERROR(ParsePath(path, &container, &object));
  as::blob_client_wrapper bc(client_);
  auto blobs = bc.list_blobs_segmented(container, "/", "", object, 1);
  if (errno != 0) {
    return Status(
        Status::Code::INTERNAL, "Failed to check if file exists at " + path +
                                    ", errno:" + strerror(errno));
  }
  if (blobs.blobs.size() > 0) {
    *exists = true;
  }
  return Status::Success;
}

Status
ASFileSystem::DownloadFolder(
    const std::string& container, const std::string& path,
    const std::string& dest)
{
  as::blob_client_wrapper bc(client_);
  auto func = [&](const as::list_blobs_segmented_item& item,
                  const std::string& dir) {
    auto local_path = JoinPath({dest, dir});
    auto blob_path = JoinPath({path, dir});
    if (item.is_directory) {
      int status = mkdir(
          const_cast<char*>(local_path.c_str()), S_IRUSR | S_IWUSR | S_IXUSR);
      if (status == -1) {
        return Status(
            Status::Code::INTERNAL,
            "Failed to create local folder: " + local_path +
                ", errno:" + strerror(errno));
      }
      auto ret = DownloadFolder(container, blob_path, local_path);
      if (!ret.IsOk()) {
        return ret;
      }
    } else {
      time_t last_modified;
      bc.download_blob_to_file(container, blob_path, local_path, last_modified);
      if (errno != 0) {
        return Status(
            Status::Code::INTERNAL, "Failed to download file at " + blob_path +
                                        ", errno:" + strerror(errno));
      }
    }
    return Status::Success;
  };
  return ListDirectory(container, path, func);
}

Status
ASFileSystem::LocalizeDirectory(
    const std::string& path, std::shared_ptr<LocalizedDirectory>* localized)
{
  bool exists;
  RETURN_IF_ERROR(FileExists(path, &exists));

  bool is_dir = false;
  if (exists) {
    RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  }
  if (!is_dir) {
    return Status(
        Status::Code::INTERNAL, "directory does not exist at " + path);
  }

  std::string folder_template = "/tmp/folderXXXXXX";
  char* tmp_folder = mkdtemp(const_cast<char*>(folder_template.c_str()));
  if (tmp_folder == nullptr) {
    return Status(
        Status::Code::INTERNAL,
        "Failed to create local temp folder: " + folder_template +
            ", errno:" + strerror(errno));
  }
  localized->reset(new LocalizedDirectory(path, tmp_folder));

  std::string dest(folder_template);

  as::blob_client_wrapper bc(client_);

  std::string container, object;
  RETURN_IF_ERROR(ParsePath(path, &container, &object));
  return DownloadFolder(container, object, dest);
}

Status
ASFileSystem::WriteTextFile(
    const std::string& path, const std::string& contents)
{
  std::stringstream ss(contents);
  std::istream is(ss.rdbuf());
  std::string container, object;
  RETURN_IF_ERROR(ParsePath(path, &container, &object));
  std::vector<std::pair<std::string, std::string>> metadata;
  auto ret =
      client_->upload_block_blob_from_stream(container, object, is, metadata)
          .get();
  if (!ret.success()) {
    return Status(
        Status::Code::INTERNAL,
        "Failed to upload blob, Error: " + ret.error().code + ", " +
            ret.error().code_name);
  }
  return Status::Success;
}

Status
ASFileSystem::WriteBinaryFile(
    const std::string& path, const char* contents, const size_t content_len)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Write text file operation not yet implemented " + path);
}

Status
ASFileSystem::MakeDirectory(const std::string& dir, const bool recursive)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Make directory operation not yet implemented");
}

Status
ASFileSystem::MakeTemporaryDirectory(std::string* temp_dir)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Make temporary directory operation not yet implemented");
}

Status
ASFileSystem::DeleteDirectory(const std::string& path)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Delete directory operation not yet implemented");
}

#endif  // TRITON_ENABLE_AZURE_STORAGE


#ifdef TRITON_ENABLE_S3

namespace s3 = Aws::S3;

class S3FileSystem final : public FileSystem {
 public:
  S3FileSystem(const std::string& s3_path);
  S3FileSystem(
      const std::string& s3_path, const char* secret_key, const char* key_id,
      const char* region, const char* session_token);
  ~S3FileSystem();

  Status CheckClient(const std::string& s3_path);
  Status FileExists(const std::string& path, bool* exists) override;
  Status IsDirectory(const std::string& path, bool* is_dir) override;
  Status FileModificationTime(
      const std::string& path, int64_t* mtime_ns) override;
  Status GetDirectoryContents(
      const std::string& path, std::set<std::string>* contents) override;
  Status GetDirectorySubdirs(
      const std::string& path, std::set<std::string>* subdirs) override;
  Status GetDirectoryFiles(
      const std::string& path, std::set<std::string>* files) override;
  Status ReadTextFile(const std::string& path, std::string* contents) override;
  Status LocalizeDirectory(
      const std::string& path,
      std::shared_ptr<LocalizedDirectory>* localized) override;
  Status WriteTextFile(
      const std::string& path, const std::string& contents) override;
  Status WriteBinaryFile(
      const std::string& path, const char* contents,
      const size_t content_len) override;
  Status MakeDirectory(const std::string& dir, const bool recursive) override;
  Status MakeTemporaryDirectory(std::string* temp_dir) override;
  Status DeleteDirectory(const std::string& path) override;

 private:
  Status ParsePath(
      const std::string& path, std::string* bucket, std::string* object);
  Status CleanPath(const std::string& s3_path, std::string* clean_path);
  const Aws::SDKOptions options_;
  std::unique_ptr<s3::S3Client> client_;  // init after Aws::InitAPI is called
  re2::RE2 s3_regex_;
  // enforce order: Aws::InitAPI -> Aws::ShutdownAPI -> Aws::InitAPI ...
  static std::mutex init_mu_;
  static size_t init_count_;
};
std::mutex S3FileSystem::init_mu_;
size_t S3FileSystem::init_count_ = 0;

Status
S3FileSystem::ParsePath(
    const std::string& path, std::string* bucket, std::string* object)
{
  // Cleanup extra slashes
  std::string clean_path;
  RETURN_IF_ERROR(CleanPath(path, &clean_path));

  // Get the bucket name and the object path. Return error if path is malformed
  std::string protocol, host_name, host_port;
  if (!RE2::FullMatch(
          clean_path, s3_regex_, &protocol, &host_name, &host_port, bucket,
          object)) {
    int bucket_start = clean_path.find("s3://") + strlen("s3://");
    int bucket_end = clean_path.find("/", bucket_start);

    // If there isn't a slash, the address has only the bucket
    if (bucket_end > bucket_start) {
      *bucket = clean_path.substr(bucket_start, bucket_end - bucket_start);
      *object = clean_path.substr(bucket_end + 1);
    } else {
      *bucket = clean_path.substr(bucket_start);
      *object = "";
    }
  } else {
    // Erase leading '/' that is left behind in object name
    if ((*object)[0] == '/') {
      object->erase(0, 1);
    }
  }

  if (bucket->empty()) {
    return Status(
        Status::Code::INTERNAL, "No bucket name found in path: " + path);
  }

  return Status::Success;
}

Status
S3FileSystem::CleanPath(const std::string& s3_path, std::string* clean_path)
{
  // Must handle paths with s3 prefix
  size_t start = s3_path.find("s3://");
  std::string path = "";
  if (start != std::string::npos) {
    path = s3_path.substr(start + strlen("s3://"));
    *clean_path = "s3://";
  } else {
    path = s3_path;
    *clean_path = "";
  }

  // Must handle paths with https:// or http:// prefix
  size_t https_start = path.find("https://");
  if (https_start != std::string::npos) {
    path = path.substr(https_start + strlen("https://"));
    *clean_path += "https://";
  } else {
    size_t http_start = path.find("http://");
    if (http_start != std::string::npos) {
      path = path.substr(http_start + strlen("http://"));
      *clean_path += "http://";
    }
  }

  // Remove trailing slashes
  size_t rtrim_length = path.find_last_not_of('/');
  if (rtrim_length == std::string::npos) {
    return Status(
        Status::Code::INVALID_ARG, "Invalid bucket name: '" + path + "'");
  }

  // Remove leading slashes
  size_t ltrim_length = path.find_first_not_of('/');
  if (ltrim_length == std::string::npos) {
    return Status(
        Status::Code::INVALID_ARG, "Invalid bucket name: '" + path + "'");
  }

  // Remove extra internal slashes
  std::string true_path = path.substr(ltrim_length, rtrim_length + 1);
  std::vector<int> slash_locations;
  bool previous_slash = false;
  for (size_t i = 0; i < true_path.size(); i++) {
    if (true_path[i] == '/') {
      if (!previous_slash) {
        *clean_path += true_path[i];
      }
      previous_slash = true;
    } else {
      *clean_path += true_path[i];
      previous_slash = false;
    }
  }

  return Status::Success;
}

S3FileSystem::S3FileSystem(const std::string& s3_path)
    : S3FileSystem(
          s3_path, std::getenv("AWS_SECRET_ACCESS_KEY"),
          std::getenv("AWS_ACCESS_KEY_ID"), std::getenv("AWS_DEFAULT_REGION"),
          std::getenv("AWS_SESSION_TOKEN"))
{
}

S3FileSystem::S3FileSystem(
    const std::string& s3_path, const char* secret_key, const char* key_id,
    const char* region, const char* session_token)
    : s3_regex_(
          "s3://(http://|https://|)([0-9a-zA-Z\\-.]+):([0-9]+)/"
          "([0-9a-z.\\-]+)(((/[0-9a-zA-Z.\\-_]+)*)?)")
{
  // init aws API if not already
  init_mu_.lock();
  if (!init_count_++) {
    Aws::InitAPI(options_);
  }
  init_mu_.unlock();

  Aws::Client::ClientConfiguration config;
  Aws::Auth::AWSCredentials credentials;

  // check vars for S3 credentials -> AWS_PROFILE (ENV) -> default
  if ((secret_key != NULL) && (key_id != NULL)) {
    credentials.SetAWSAccessKeyId(key_id);
    credentials.SetAWSSecretKey(secret_key);
    if (session_token != NULL) {
      credentials.SetSessionToken(session_token);
    }
    config = Aws::Client::ClientConfiguration();
    if (region != NULL) {
      config.region = region;
    }
  } else if (const char* profile_name = std::getenv("AWS_PROFILE")) {
    config = Aws::Client::ClientConfiguration(profile_name);
  } else {
    config = Aws::Client::ClientConfiguration("default");
  }

  // Cleanup extra slashes
  std::string clean_path;
  LOG_STATUS_ERROR(CleanPath(s3_path, &clean_path), "failed to parse S3 path");

  std::string protocol, host_name, host_port, bucket, object;
  if (RE2::FullMatch(
          clean_path, s3_regex_, &protocol, &host_name, &host_port, &bucket,
          &object)) {
    config.endpointOverride = Aws::String(host_name + ":" + host_port);
    if (protocol == "https://") {
      config.scheme = Aws::Http::Scheme::HTTPS;
    } else {
      config.scheme = Aws::Http::Scheme::HTTP;
    }
  }

  if ((secret_key != NULL) && (key_id != NULL)) {
    client_.reset(new s3::S3Client(
        credentials, config,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        /*useVirtualAdressing*/ false));

  } else {
    client_.reset(new s3::S3Client(
        config, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        /*useVirtualAdressing*/ false));
  }
}

S3FileSystem::~S3FileSystem()
{
  // shutdown aws API if no more active instance
  std::lock_guard<std::mutex> lock(init_mu_);
  if (!--init_count_) {
    Aws::ShutdownAPI(options_);
  }
}

Status
S3FileSystem::CheckClient(const std::string& s3_path)
{
  bool b;
  if (!IsDirectory(s3_path, &b).IsOk()) {
    return Status(
        Status::Code::INTERNAL,
        "Unable to create S3 filesystem client. Check account credentials.");
  }
  return Status::Success;
}

Status
S3FileSystem::FileExists(const std::string& path, bool* exists)
{
  *exists = false;

  // S3 doesn't make objects for directories, so it could still be a directory
  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  if (is_dir) {
    *exists = is_dir;
    return Status::Success;
  }

  std::string bucket, object;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object));

  // Construct request for object metadata
  s3::Model::HeadObjectRequest head_request;
  head_request.SetBucket(bucket.c_str());
  head_request.SetKey(object.c_str());

  auto head_object_outcome = client_->HeadObject(head_request);
  if (!head_object_outcome.IsSuccess()) {
    if (head_object_outcome.GetError().GetErrorType() !=
        s3::S3Errors::RESOURCE_NOT_FOUND) {
      return Status(
          Status::Code::INTERNAL,
          "Could not get MetaData for object at " + path +
              " due to exception: " +
              head_object_outcome.GetError().GetExceptionName() +
              ", error message: " +
              head_object_outcome.GetError().GetMessage());
    }
  } else {
    *exists = true;
  }

  return Status::Success;
}

Status
S3FileSystem::IsDirectory(const std::string& path, bool* is_dir)
{
  *is_dir = false;
  std::string bucket, object_path;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object_path));

  // Check if the bucket exists
  s3::Model::HeadBucketRequest head_request;
  head_request.WithBucket(bucket.c_str());

  auto head_bucket_outcome = client_->HeadBucket(head_request);
  if (!head_bucket_outcome.IsSuccess()) {
    return Status(
        Status::Code::INTERNAL,
        "Could not get MetaData for bucket with name " + bucket +
            " due to exception: " +
            head_bucket_outcome.GetError().GetExceptionName() +
            ", error message: " + head_bucket_outcome.GetError().GetMessage());
  }

  // Root case - bucket exists and object path is empty
  if (object_path.empty()) {
    *is_dir = true;
    return Status::Success;
  }

  // List the objects in the bucket
  s3::Model::ListObjectsRequest list_objects_request;
  list_objects_request.SetBucket(bucket.c_str());
  list_objects_request.SetPrefix(AppendSlash(object_path).c_str());
  auto list_objects_outcome = client_->ListObjects(list_objects_request);

  if (list_objects_outcome.IsSuccess()) {
    *is_dir = !list_objects_outcome.GetResult().GetContents().empty();
  } else {
    return Status(
        Status::Code::INTERNAL,
        "Failed to list objects with prefix " + path + " due to exception: " +
            list_objects_outcome.GetError().GetExceptionName() +
            ", error message: " + list_objects_outcome.GetError().GetMessage());
  }
  return Status::Success;
}

Status
S3FileSystem::FileModificationTime(const std::string& path, int64_t* mtime_ns)
{
  // We don't need to worry about the case when this is a directory
  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  if (is_dir) {
    *mtime_ns = 0;
    return Status::Success;
  }

  std::string bucket, object;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object));

  // Send a request for the objects metadata
  s3::Model::HeadObjectRequest head_request;
  head_request.SetBucket(bucket.c_str());
  head_request.SetKey(object.c_str());

  // If request succeeds, copy over the modification time
  auto head_object_outcome = client_->HeadObject(head_request);
  if (head_object_outcome.IsSuccess()) {
    *mtime_ns = head_object_outcome.GetResult().GetLastModified().Millis() *
                NANOS_PER_MILLIS;
  } else {
    return Status(
        Status::Code::INTERNAL,
        "Failed to get modification time for object at " + path +
            " due to exception: " +
            head_object_outcome.GetError().GetExceptionName() +
            ", error message: " + head_object_outcome.GetError().GetMessage());
  }
  return Status::Success;
}

Status
S3FileSystem::GetDirectoryContents(
    const std::string& path, std::set<std::string>* contents)
{
  // Parse bucket and dir_path
  std::string bucket, dir_path, full_dir;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &dir_path));
  std::string true_path = "s3://" + bucket + '/' + dir_path;

  // Capture the full path to facilitate content listing
  full_dir = AppendSlash(dir_path);

  // Issue request for objects with prefix
  s3::Model::ListObjectsRequest objects_request;
  objects_request.SetBucket(bucket.c_str());
  objects_request.SetPrefix(full_dir.c_str());
  auto list_objects_outcome = client_->ListObjects(objects_request);

  if (list_objects_outcome.IsSuccess()) {
    Aws::Vector<Aws::S3::Model::Object> object_list =
        list_objects_outcome.GetResult().GetContents();
    for (auto const& s3_object : object_list) {
      // In the case of empty directories, the directory itself will appear here
      if (s3_object.GetKey().c_str() == full_dir) {
        continue;
      }

      // We have to make sure that subdirectory contents do not appear here
      std::string name(s3_object.GetKey().c_str());
      int item_start = name.find(full_dir) + full_dir.size();
      // S3 response prepends parent directory name
      int item_end = name.find("/", item_start);

      // Let set take care of subdirectory contents
      std::string item = name.substr(item_start, item_end - item_start);
      contents->insert(item);
    }
  } else {
    return Status(
        Status::Code::INTERNAL,
        "Could not list contents of directory at " + true_path +
            " due to exception: " +
            list_objects_outcome.GetError().GetExceptionName() +
            ", error message: " + list_objects_outcome.GetError().GetMessage());
  }
  return Status::Success;
}

Status
S3FileSystem::GetDirectorySubdirs(
    const std::string& path, std::set<std::string>* subdirs)
{
  // Parse bucket and dir_path
  std::string bucket, dir_path;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &dir_path));
  std::string true_path = "s3://" + bucket + '/' + dir_path;

  RETURN_IF_ERROR(GetDirectoryContents(true_path, subdirs));

  // Erase non-directory entries...
  for (auto iter = subdirs->begin(); iter != subdirs->end();) {
    bool is_dir;
    RETURN_IF_ERROR(IsDirectory(JoinPath({true_path, *iter}), &is_dir));
    if (!is_dir) {
      iter = subdirs->erase(iter);
    } else {
      ++iter;
    }
  }

  return Status::Success;
}
Status
S3FileSystem::GetDirectoryFiles(
    const std::string& path, std::set<std::string>* files)
{
  // Parse bucket and dir_path
  std::string bucket, dir_path;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &dir_path));
  std::string true_path = "s3://" + bucket + '/' + dir_path;
  RETURN_IF_ERROR(GetDirectoryContents(true_path, files));

  // Erase directory entries...
  for (auto iter = files->begin(); iter != files->end();) {
    bool is_dir;
    RETURN_IF_ERROR(IsDirectory(JoinPath({true_path, *iter}), &is_dir));
    if (is_dir) {
      iter = files->erase(iter);
    } else {
      ++iter;
    }
  }

  return Status::Success;
}

Status
S3FileSystem::ReadTextFile(const std::string& path, std::string* contents)
{
  bool exists;
  RETURN_IF_ERROR(FileExists(path, &exists));

  if (!exists) {
    return Status(Status::Code::INTERNAL, "File does not exist at " + path);
  }

  std::string bucket, object;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object));

  // Send a request for the objects metadata
  s3::Model::GetObjectRequest object_request;
  object_request.SetBucket(bucket.c_str());
  object_request.SetKey(object.c_str());

  auto get_object_outcome = client_->GetObject(object_request);
  if (get_object_outcome.IsSuccess()) {
    auto& object_result = get_object_outcome.GetResultWithOwnership().GetBody();

    std::string data = "";
    char c;
    while (object_result.get(c)) {
      data += c;
    }

    *contents = data;
  } else {
    return Status(
        Status::Code::INTERNAL,
        "Failed to get object at " + path + " due to exception: " +
            get_object_outcome.GetError().GetExceptionName() +
            ", error message: " + get_object_outcome.GetError().GetMessage());
  }

  return Status::Success;
}

Status
S3FileSystem::LocalizeDirectory(
    const std::string& path, std::shared_ptr<LocalizedDirectory>* localized)
{
  bool exists;
  RETURN_IF_ERROR(FileExists(path, &exists));

  bool is_dir = false;
  if (exists) {
    RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  }

  if (!is_dir) {
    return Status(
        Status::Code::INTERNAL, "directory does not exist at " + path);
  }

  // Cleanup extra slashes
  std::string clean_path;
  RETURN_IF_ERROR(CleanPath(path, &clean_path));

  std::string effective_path, protocol, host_name, host_port, bucket, object;
  if (RE2::FullMatch(
          clean_path, s3_regex_, &protocol, &host_name, &host_port, &bucket,
          &object)) {
    effective_path = "s3://" + bucket + object;
  } else {
    effective_path = path;
  }

  std::string tmp_folder;
  RETURN_IF_ERROR(
      triton::core::MakeTemporaryDirectory(FileSystemType::LOCAL, &tmp_folder));

  localized->reset(new LocalizedDirectory(effective_path, tmp_folder));

  std::set<std::string> contents, filenames;
  RETURN_IF_ERROR(GetDirectoryContents(effective_path, &filenames));
  for (auto itr = filenames.begin(); itr != filenames.end(); ++itr) {
    contents.insert(JoinPath({effective_path, *itr}));
  }

  while (contents.size() != 0) {
    std::set<std::string> tmp_contents = contents;
    contents.clear();
    for (auto iter = tmp_contents.begin(); iter != tmp_contents.end(); ++iter) {
      bool is_subdir;
      std::string s3_fpath = *iter;
      std::string s3_removed_path = s3_fpath.substr(effective_path.size());
      std::string local_fpath =
          JoinPath({(*localized)->Path(), s3_removed_path});
      RETURN_IF_ERROR(IsDirectory(s3_fpath, &is_subdir));
      if (is_subdir) {
        // Create local mirror of sub-directories
#ifdef _WIN32
        int status = mkdir(const_cast<char*>(local_fpath.c_str()));
#else
        int status = mkdir(
            const_cast<char*>(local_fpath.c_str()),
            S_IRUSR | S_IWUSR | S_IXUSR);
#endif
        if (status == -1) {
          return Status(
              Status::Code::INTERNAL,
              "Failed to create local folder: " + local_fpath +
                  ", errno:" + strerror(errno));
        }

        // Add sub-directories and deeper files to contents
        std::set<std::string> subdir_contents;
        RETURN_IF_ERROR(GetDirectoryContents(s3_fpath, &subdir_contents));
        for (auto itr = subdir_contents.begin(); itr != subdir_contents.end();
             ++itr) {
          contents.insert(JoinPath({s3_fpath, *itr}));
        }
      } else {
        // Create local copy of file
        std::string file_bucket, file_object;
        RETURN_IF_ERROR(ParsePath(s3_fpath, &file_bucket, &file_object));

        s3::Model::GetObjectRequest object_request;
        object_request.SetBucket(file_bucket.c_str());
        object_request.SetKey(file_object.c_str());

        auto get_object_outcome = client_->GetObject(object_request);
        if (get_object_outcome.IsSuccess()) {
          auto& retrieved_file =
              get_object_outcome.GetResultWithOwnership().GetBody();
          std::ofstream output_file(local_fpath.c_str(), std::ios::binary);
          output_file << retrieved_file.rdbuf();
          output_file.close();
        } else {
          return Status(
              Status::Code::INTERNAL,
              "Failed to get object at " + s3_fpath + " due to exception: " +
                  get_object_outcome.GetError().GetExceptionName() +
                  ", error message: " +
                  get_object_outcome.GetError().GetMessage());
        }
      }
    }
  }

  return Status::Success;
}

Status
S3FileSystem::WriteTextFile(
    const std::string& path, const std::string& contents)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Write text file operation not yet implemented " + path);
}

Status
S3FileSystem::WriteBinaryFile(
    const std::string& path, const char* contents, const size_t content_len)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Write text file operation not yet implemented " + path);
}

Status
S3FileSystem::MakeDirectory(const std::string& dir, const bool recursive)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Make directory operation not yet implemented");
}

Status
S3FileSystem::MakeTemporaryDirectory(std::string* temp_dir)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Make temporary directory operation not yet implemented");
}

Status
S3FileSystem::DeleteDirectory(const std::string& path)
{
  return Status(
      Status::Code::UNSUPPORTED,
      "Delete directory operation not yet implemented");
}


#endif  // TRITON_ENABLE_S3


class FileSystemManager {
 public:
  // create a FileSystem from the given path
  // the class instance is to keep track and auto delete unused FileSystem
  // the FileSystem will live for the duration of this class instance
  // use a different class instance for different paths
  Status GetFileSystem(const std::string& path, FileSystem** file_system);
  // create a FileSystem from the given type
  Status GetFileSystem(FileSystemType type, FileSystem** file_system);

 private:
  Status ReturnErrorOrReload(
      const Status& load_status, const Status& error_status,
      const std::string& path, FileSystem** file_system);

  static Status LoadCredential(bool flush_cache = false);

  template <class FileSystemCredential>
  static void SortCredential(
      std::vector<std::pair<std::string, FileSystemCredential>>& credential);
  template <class FileSystemCredential>
  static Status GetLongestMatchingCredential(
      const std::vector<std::pair<std::string, FileSystemCredential>>&
          credentials,
      const std::string& file_system_prefix, const std::string& path,
      FileSystemCredential& credential);

  // for protecting concurrent access into static variables
  static std::mutex mu_;
  static bool is_credential_cached_;
  // credentials should be sorted in descending name length order
#ifdef TRITON_ENABLE_GCS
  static std::vector<std::pair<std::string, std::string>> gs_credential_;
  std::unique_ptr<GCSFileSystem> gcs_fs_;
#endif  // TRITON_ENABLE_GCS
#ifdef TRITON_ENABLE_S3
  static std::vector<std::pair<
      std::string,
      std::tuple<std::string, std::string, std::string, std::string>>>
      s3_credential_;
  std::unique_ptr<S3FileSystem> s3_fs_;
#endif  // TRITON_ENABLE_S3
#ifdef TRITON_ENABLE_AZURE_STORAGE
  static std::vector<
      std::pair<std::string, std::pair<std::string, std::string>>>
      as_credential_;
  std::unique_ptr<ASFileSystem> as_fs_;
#endif  // TRITON_ENABLE_AZURE_STORAGE
  std::unique_ptr<LocalFileSystem> local_fs_;
};
// initialize FileSystemManager static variables
std::mutex FileSystemManager::mu_;
bool FileSystemManager::is_credential_cached_ = false;
#ifdef TRITON_ENABLE_GCS
std::vector<std::pair<std::string, std::string>>
    FileSystemManager::gs_credential_;
#endif  // TRITON_ENABLE_GCS
#ifdef TRITON_ENABLE_S3
std::vector<std::pair<
    std::string,
    std::tuple<std::string, std::string, std::string, std::string>>>
    FileSystemManager::s3_credential_;
#endif  // TRITON_ENABLE_S3
#ifdef TRITON_ENABLE_AZURE_STORAGE
std::vector<std::pair<std::string, std::pair<std::string, std::string>>>
    FileSystemManager::as_credential_;
#endif  // TRITON_ENABLE_AZURE_STORAGE

Status
FileSystemManager::GetFileSystem(
    const std::string& path, FileSystem** file_system)
{
  // Check if this is a GCS path (gs://$BUCKET_NAME)
  if (!path.empty() && !path.rfind("gs://", 0)) {
#ifndef TRITON_ENABLE_GCS
    return Status(
        Status::Code::INTERNAL,
        "gs:// file-system not supported. To enable, build with "
        "-DTRITON_ENABLE_GCS=ON.");
#else
    const Status& load_status = LoadCredential();
    if (load_status.IsOk() ||
        load_status.StatusCode() == Status::Code::ALREADY_EXISTS) {
      std::string credential_path;
      const Status& match_status = GetLongestMatchingCredential<std::string>(
          gs_credential_, "gs://", path.substr(5, std::string::npos),
          credential_path);
      if (!match_status.IsOk()) {
        return ReturnErrorOrReload(
            load_status, match_status, path, file_system);
      }
      gcs_fs_.reset(new GCSFileSystem(credential_path));
      const Status& client_status = gcs_fs_->CheckClient();
      if (!client_status.IsOk()) {
        return ReturnErrorOrReload(
            load_status, client_status, path, file_system);
      }
      *file_system = gcs_fs_.get();
      return Status::Success;
    } else if (load_status.StatusCode() == Status::Code::UNAVAILABLE) {
      gcs_fs_.reset(new GCSFileSystem());
      RETURN_IF_ERROR(gcs_fs_->CheckClient());
      *file_system = gcs_fs_.get();
      return Status::Success;
    }
    return load_status;
#endif  // TRITON_ENABLE_GCS
  }

  // Check if this is an S3 path (s3://$BUCKET_NAME)
  if (!path.empty() && !path.rfind("s3://", 0)) {
#ifndef TRITON_ENABLE_S3
    return Status(
        Status::Code::INTERNAL,
        "s3:// file-system not supported. To enable, build with "
        "-DTRITON_ENABLE_S3=ON.");
#else
    const Status& load_status = LoadCredential();
    if (load_status.IsOk() ||
        load_status.StatusCode() == Status::Code::ALREADY_EXISTS) {
      std::tuple<std::string, std::string, std::string, std::string>
          credential_;
      const Status& match_status = GetLongestMatchingCredential<
          std::tuple<std::string, std::string, std::string, std::string>>(
          s3_credential_, "s3://", path.substr(5, std::string::npos),
          credential_);
      if (!match_status.IsOk()) {
        return ReturnErrorOrReload(
            load_status, match_status, path, file_system);
      }
      s3_fs_.reset(new S3FileSystem(
          path, std::get<0>(credential_).c_str(),
          std::get<1>(credential_).c_str(), std::get<2>(credential_).c_str(),
          std::get<3>(credential_).size() ? std::get<3>(credential_).c_str()
                                          : nullptr));
      const Status& client_status = s3_fs_->CheckClient(path);
      if (!client_status.IsOk()) {
        return ReturnErrorOrReload(
            load_status, client_status, path, file_system);
      }
      *file_system = s3_fs_.get();
      return Status::Success;
    } else if (load_status.StatusCode() == Status::Code::UNAVAILABLE) {
      s3_fs_.reset(new S3FileSystem(path));
      RETURN_IF_ERROR(s3_fs_->CheckClient(path));
      *file_system = s3_fs_.get();
      return Status::Success;
    }
    return load_status;
#endif  // TRITON_ENABLE_S3
  }

  // Check if this is an Azure Storage path
  if (!path.empty() && !path.rfind("as://", 0)) {
#ifndef TRITON_ENABLE_AZURE_STORAGE
    return Status(
        Status::Code::INTERNAL,
        "as:// file-system not supported. To enable, build with "
        "-DTRITON_ENABLE_AZURE_STORAGE=ON.");
#else
    const Status& load_status = LoadCredential();
    if (load_status.IsOk() ||
        load_status.StatusCode() == Status::Code::ALREADY_EXISTS) {
      std::pair<std::string, std::string> credential_;
      const Status& match_status =
          GetLongestMatchingCredential<std::pair<std::string, std::string>>(
              as_credential_, "as://", path.substr(5, std::string::npos),
              credential_);
      if (!match_status.IsOk()) {
        return ReturnErrorOrReload(
            load_status, match_status, path, file_system);
      }
      as_fs_.reset(new ASFileSystem(
          path, credential_.first.c_str(), credential_.second.c_str()));
      const Status& client_status = as_fs_->CheckClient();
      if (!client_status.IsOk()) {
        return ReturnErrorOrReload(
            load_status, client_status, path, file_system);
      }
      *file_system = as_fs_.get();
      return Status::Success;
    } else if (load_status.StatusCode() == Status::Code::UNAVAILABLE) {
      as_fs_.reset(new ASFileSystem(path));
      RETURN_IF_ERROR(as_fs_->CheckClient());
      *file_system = as_fs_.get();
      return Status::Success;
    }
    return load_status;
#endif  // TRITON_ENABLE_AZURE_STORAGE
  }

  // Assume path is for local filesystem
  local_fs_.reset(new LocalFileSystem());
  *file_system = local_fs_.get();
  return Status::Success;
}

Status
FileSystemManager::GetFileSystem(FileSystemType type, FileSystem** file_system)
{
  const Status& status = LoadCredential();
  if (status.IsOk() || status.StatusCode() == Status::Code::ALREADY_EXISTS) {
    // with credential file, use default credential (one with shortest name)
    switch (type) {
      case FileSystemType::LOCAL:
        return GetFileSystem("", file_system);
      case FileSystemType::GCS:
        return GetFileSystem("gs://", file_system);
      case FileSystemType::S3:
        return GetFileSystem("s3://", file_system);
      case FileSystemType::AS:
        return GetFileSystem("as://", file_system);
      default:
        return Status(Status::Code::UNSUPPORTED, "Unsupported filesystem type");
    }
  } else if (status.StatusCode() == Status::Code::UNAVAILABLE) {
    // with env var, only LOCAL and GCS are not path-dependent
    switch (type) {
      case FileSystemType::LOCAL:
        return GetFileSystem("", file_system);
      case FileSystemType::GCS:
        return GetFileSystem("gs://", file_system);
      case FileSystemType::S3:
        return Status(
            Status::Code::UNSUPPORTED,
            "S3 filesystem cannot be accessed by type with environment "
            "variable credential");
      case FileSystemType::AS:
        return Status(
            Status::Code::UNSUPPORTED,
            "AS filesystem cannot be accessed by type with environment "
            "variable credential");
      default:
        return Status(Status::Code::UNSUPPORTED, "Unsupported filesystem type");
    }
  }
  return status;
}

Status
FileSystemManager::ReturnErrorOrReload(
    const Status& load_status, const Status& error_status,
    const std::string& path, FileSystem** file_system)
{
  if (load_status.StatusCode() == Status::Code::ALREADY_EXISTS) {
    return error_status;
  }
  LoadCredential(true);  // flush cache
  return GetFileSystem(path, file_system);
}

// meaning of return status:
// - Status(SUCCESS) -> loaded credential from file
// - Status(ALREADY_EXISTS, "Cached") -> credential already loaded
// - Status(UNAVAILABLE, "Use legacy credential") -> no credential file
Status
FileSystemManager::LoadCredential(bool flush_cache)
{
  // prevent concurrent access into class variables
  std::lock_guard<std::mutex> lock(mu_);

  // check if credential is already cached
  if (is_credential_cached_ && !flush_cache) {
    return Status(Status::Code::ALREADY_EXISTS, "Cached");
  }

  const char* file_path_c_str = std::getenv("TRITON_CLOUD_CREDENTIAL_PATH");
  if (file_path_c_str != nullptr) {
    std::string file_path = std::string(file_path_c_str);
    LOG_VERBOSE(1) << "Reading cloud credential from " << file_path;

    triton::common::TritonJson::Value creds_json;
    std::string cred_file_content;
    LocalFileSystem fs;
    RETURN_IF_ERROR(fs.ReadTextFile(file_path, &cred_file_content));
    RETURN_IF_ERROR(creds_json.Parse(cred_file_content));

#ifdef TRITON_ENABLE_GCS
    // load GCS credentials
    gs_credential_.clear();
    triton::common::TritonJson::Value gs_creds_json;
    if (creds_json.Find("gs", &gs_creds_json)) {
      std::vector<std::string> gs_cred_names;
      gs_creds_json.Members(&gs_cred_names);
      for (size_t i = 0; i < gs_cred_names.size(); i++) {
        std::string gs_cred_name = gs_cred_names[i];
        std::string gs_cred_path;
        triton::common::TritonJson::Value gs_cred_path_json;
        gs_creds_json.Find(gs_cred_name.c_str(), &gs_cred_path_json);
        gs_cred_path_json.AsString(&gs_cred_path);
        gs_credential_.push_back(std::make_pair(gs_cred_name, gs_cred_path));
      }
      SortCredential(gs_credential_);
    }
#endif  // TRITON_ENABLE_GCS

#ifdef TRITON_ENABLE_S3
    // load S3 credentials
    s3_credential_.clear();
    triton::common::TritonJson::Value s3_creds_json;
    if (creds_json.Find("s3", &s3_creds_json)) {
      std::vector<std::string> s3_cred_names;
      s3_creds_json.Members(&s3_cred_names);
      for (size_t i = 0; i < s3_cred_names.size(); i++) {
        std::string s3_cred_name = s3_cred_names[i];
        std::string s3_cred_secret_key;
        std::string s3_cred_key_id;
        std::string s3_cred_region;
        std::string s3_cred_session_token;
        triton::common::TritonJson::Value s3_cred_json;
        s3_creds_json.Find(s3_cred_name.c_str(), &s3_cred_json);
        triton::common::TritonJson::Value s3_cred_secret_key_json;
        triton::common::TritonJson::Value s3_cred_key_id_json;
        triton::common::TritonJson::Value s3_cred_region_json;
        triton::common::TritonJson::Value s3_cred_session_token_json;
        if (s3_cred_json.Find("secret_key", &s3_cred_secret_key_json))
          s3_cred_secret_key_json.AsString(&s3_cred_secret_key);
        if (s3_cred_json.Find("key_id", &s3_cred_key_id_json))
          s3_cred_key_id_json.AsString(&s3_cred_key_id);
        if (s3_cred_json.Find("region", &s3_cred_region_json))
          s3_cred_region_json.AsString(&s3_cred_region);
        if (s3_cred_json.Find("session_token", &s3_cred_session_token_json))
          s3_cred_session_token_json.AsString(&s3_cred_session_token);
        s3_credential_.push_back(std::make_pair(
            s3_cred_name, std::make_tuple(
                              s3_cred_secret_key, s3_cred_key_id,
                              s3_cred_region, s3_cred_session_token)));
      }
      SortCredential(s3_credential_);
    }
#endif  // TRITON_ENABLE_S3

#ifdef TRITON_ENABLE_AZURE_STORAGE
    // load AS credentials
    as_credential_.clear();
    triton::common::TritonJson::Value as_creds_json;
    if (creds_json.Find("as", &as_creds_json)) {
      std::vector<std::string> as_cred_names;
      as_creds_json.Members(&as_cred_names);
      for (size_t i = 0; i < as_cred_names.size(); i++) {
        std::string as_cred_name = as_cred_names[i];
        std::string as_cred_account_str;
        std::string as_cred_account_key;
        triton::common::TritonJson::Value as_cred_json;
        as_creds_json.Find(as_cred_name.c_str(), &as_cred_json);
        triton::common::TritonJson::Value as_cred_account_str_json;
        triton::common::TritonJson::Value as_cred_account_key_json;
        if (as_cred_json.Find("account_str", &as_cred_account_str_json))
          as_cred_account_str_json.AsString(&as_cred_account_str);
        if (as_cred_json.Find("account_key", &as_cred_account_key_json))
          as_cred_account_key_json.AsString(&as_cred_account_key);
        as_credential_.push_back(std::make_pair(
            as_cred_name,
            std::make_pair(as_cred_account_str, as_cred_account_key)));
      }
      SortCredential(as_credential_);
    }
#endif  // TRITON_ENABLE_AZURE_STORAGE

    is_credential_cached_ = true;
    return Status::Success;
  }

  LOG_VERBOSE(1)
      << "TRITON_CLOUD_CREDENTIAL_PATH environment variable is not set";
  return Status(Status::Code::UNAVAILABLE, "Use legacy credential");
}

template <class FileSystemCredential>
void
FileSystemManager::SortCredential(
    std::vector<std::pair<std::string, FileSystemCredential>>& credential)
{
  std::sort(
      credential.begin(), credential.end(),
      [](std::pair<std::string, FileSystemCredential> i,
         std::pair<std::string, FileSystemCredential> j) {
        return i.first.size() >= j.first.size();
      });
}

template <class FileSystemCredential>
Status
FileSystemManager::GetLongestMatchingCredential(
    const std::vector<std::pair<std::string, FileSystemCredential>>&
        credentials,
    const std::string& file_system_prefix, const std::string& path,
    FileSystemCredential& credential)
{
  for (size_t i = 0; i < credentials.size(); i++) {
    if (!path.rfind(credentials[i].first, 0)) {
      credential = credentials[i].second;
      LOG_VERBOSE(1) << "Using credential  " + credentials[i].first +
                            "  for path  " + file_system_prefix + path;
      return Status::Success;
    }
  }
  return Status(
      Status::Code::NOT_FOUND,
      "Cannot match credential for path  " + file_system_prefix + path);
}

}  // namespace

// FIXME: Windows support '/'? If so, the below doesn't need to change
bool
IsAbsolutePath(const std::string& path)
{
  return !path.empty() && (path[0] == '/');
}

std::string
JoinPath(std::initializer_list<std::string> segments)
{
  std::string joined;

  for (const auto& seg : segments) {
    if (joined.empty()) {
      joined = seg;
    } else if (IsAbsolutePath(seg)) {
      if (joined[joined.size() - 1] == '/') {
        joined.append(seg.substr(1));
      } else {
        joined.append(seg);
      }
    } else {  // !IsAbsolutePath(seg)
      if (joined[joined.size() - 1] != '/') {
        joined.append("/");
      }
      joined.append(seg);
    }
  }

  return joined;
}

std::string
BaseName(const std::string& path)
{
  if (path.empty()) {
    return path;
  }

  size_t last = path.size() - 1;
  while ((last > 0) && (path[last] == '/')) {
    last -= 1;
  }

  if (path[last] == '/') {
    return std::string();
  }

  const size_t idx = path.find_last_of("/", last);
  if (idx == std::string::npos) {
    return path.substr(0, last + 1);
  }

  return path.substr(idx + 1, last - idx);
}

std::string
DirName(const std::string& path)
{
  if (path.empty()) {
    return path;
  }

  size_t last = path.size() - 1;
  while ((last > 0) && (path[last] == '/')) {
    last -= 1;
  }

  if (path[last] == '/') {
    return std::string("/");
  }

  const size_t idx = path.find_last_of("/", last);
  if (idx == std::string::npos) {
    return std::string(".");
  }
  if (idx == 0) {
    return std::string("/");
  }

  return path.substr(0, idx);
}

Status
FileExists(const std::string& path, bool* exists)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->FileExists(path, exists);
}

Status
IsDirectory(const std::string& path, bool* is_dir)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->IsDirectory(path, is_dir);
}

Status
FileModificationTime(const std::string& path, int64_t* mtime_ns)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->FileModificationTime(path, mtime_ns);
}

Status
GetDirectoryContents(const std::string& path, std::set<std::string>* contents)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->GetDirectoryContents(path, contents);
}

Status
GetDirectorySubdirs(const std::string& path, std::set<std::string>* subdirs)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->GetDirectorySubdirs(path, subdirs);
}

Status
GetDirectoryFiles(
    const std::string& path, const bool skip_hidden_files,
    std::set<std::string>* files)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  std::set<std::string> all_files;
  RETURN_IF_ERROR(fs->GetDirectoryFiles(path, &all_files));
  // Remove the hidden files
  for (auto f : all_files) {
    if ((f[0] != '.') || (!skip_hidden_files)) {
      files->insert(f);
    }
  }
  return Status::Success;
}

Status
ReadTextFile(const std::string& path, std::string* contents)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->ReadTextFile(path, contents);
}

Status
ReadTextProto(const std::string& path, google::protobuf::Message* msg)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));

  std::string contents;
  RETURN_IF_ERROR(fs->ReadTextFile(path, &contents));

  if (!google::protobuf::TextFormat::ParseFromString(contents, msg)) {
    return Status(
        Status::Code::INTERNAL, "failed to read text proto from " + path);
  }

  return Status::Success;
}

Status
LocalizeDirectory(
    const std::string& path, std::shared_ptr<LocalizedDirectory>* localized)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->LocalizeDirectory(path, localized);
}

Status
WriteTextProto(const std::string& path, const google::protobuf::Message& msg)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));

  std::string prototxt;
  if (!google::protobuf::TextFormat::PrintToString(msg, &prototxt)) {
    return Status(
        Status::Code::INTERNAL, "failed to write text proto to " + path);
  }

  return fs->WriteTextFile(path, prototxt);
}

Status
WriteBinaryFile(
    const std::string& path, const char* contents, const size_t content_len)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->WriteBinaryFile(path, contents, content_len);
}

Status
ReadBinaryProto(const std::string& path, google::protobuf::MessageLite* msg)
{
  std::string msg_str;
  RETURN_IF_ERROR(ReadTextFile(path, &msg_str));

  google::protobuf::io::CodedInputStream coded_stream(
      reinterpret_cast<const uint8_t*>(msg_str.c_str()), msg_str.size());
  coded_stream.SetTotalBytesLimit(INT_MAX, INT_MAX);
  if (!msg->ParseFromCodedStream(&coded_stream)) {
    return Status(
        Status::Code::INTERNAL, "Can't parse " + path + " as binary proto");
  }

  return Status::Success;
}

Status
MakeDirectory(const std::string& dir, const bool recursive)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(dir, &fs));
  return fs->MakeDirectory(dir, recursive);
}

Status
MakeTemporaryDirectory(const FileSystemType type, std::string* temp_dir)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(type, &fs));
  return fs->MakeTemporaryDirectory(temp_dir);
}

Status
DeleteDirectory(const std::string& path)
{
  FileSystem* fs;
  FileSystemManager fsm;
  RETURN_IF_ERROR(fsm.GetFileSystem(path, &fs));
  return fs->DeleteDirectory(path);
}

Status
GetFileSystemType(const std::string& path, FileSystemType* type)
{
  if (path.empty()) {
    return Status(
        Status::Code::INVALID_ARG,
        "Can not infer filesystem type from empty path");
  }
#ifdef TRITON_ENABLE_GCS
  // Check if this is a GCS path (gs://$BUCKET_NAME)
  if (!path.rfind("gs://", 0)) {
    *type = FileSystemType::GCS;
    return Status::Success;
  }
#endif  // TRITON_ENABLE_GCS

#ifdef TRITON_ENABLE_S3
  // Check if this is an S3 path (s3://$BUCKET_NAME)
  if (!path.rfind("s3://", 0)) {
    *type = FileSystemType::S3;
    return Status::Success;
  }
#endif  // TRITON_ENABLE_S3

#ifdef TRITON_ENABLE_AZURE_STORAGE
  // Check if this is an Azure Storage path
  if (!path.rfind("as://", 0)) {
    *type = FileSystemType::AS;
    return Status::Success;
  }
#endif  // TRITON_ENABLE_AZURE_STORAGE

  // Assume path is for local filesystem
  *type = FileSystemType::LOCAL;
  return Status::Success;
}

const std::string&
FileSystemTypeString(const FileSystemType type)
{
  static const std::string local_str("LOCAL");
  static const std::string gcs_str("GCS");
  static const std::string s3_str("S3");
  static const std::string as_str("AS");
  static const std::string unknown_str("UNKNOWN");
  switch (type) {
    case FileSystemType::LOCAL:
      return local_str;
    case FileSystemType::GCS:
      return gcs_str;
    case FileSystemType::S3:
      return s3_str;
    case FileSystemType::AS:
      return as_str;
    default:
      return unknown_str;
  }
}

}}  // namespace triton::core

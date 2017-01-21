/*
 * Copyright 2015 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "localfs/localfs.h"
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <string>

#include "config/config.h"
#include "localfs/localfs-config-vars.h"

namespace heron {
namespace ckptmgr {

LocalFS::LocalFS(const heron::config::Config& _config) {
  std::string stype = _config.getstr(heron::config::StatefulConfigVars::STORAGE_TYPE);
  CHECK_EQ(storage_type(), stype);

  // get the root directory for storing checkpoints
  base_dir_ = _config.getstr(LocalfsConfigVars::ROOT_DIR);
  LOG_IF(FATAL, base_dir_.empty()) << "Local File System root directory not set";
}

std::string LocalFS::ckptDirectory(const Checkpoint& _ckpt) {
  std::string directory(base_dir_ + "/");
  directory.append(_ckpt.getCkptId()).append("/");
  directory.append(_ckpt.getComponent());
  return directory;
}

std::string LocalFS::ckptFile(const Checkpoint& _ckpt) {
  std::string directory(ckptDirectory(_ckpt) + "/");
  return directory.append(_ckpt.getTaskId());
}

std::string LocalFS::tempCkptFile(const Checkpoint& _ckpt) {
  std::string directory(ckptDirectory(_ckpt) + "/");
  return directory.append(".").append(_ckpt.getTaskId());
}

std::string LocalFS::logMessageFragment(const Checkpoint& _ckpt) {
  std::string message(_ckpt.getTopology() + " ");
  message.append(_ckpt.getCkptId()).append(" ");
  message.append(_ckpt.getComponent()).append(" ");
  message.append(_ckpt.getInstance()).append(" ");
  return message;
}

int LocalFS::createCkptDirectory(const Checkpoint& _ckpt) {
  std::string directory = ckptDirectory(_ckpt);
  if (FileUtils::makePath(directory) != SP_OK) {
    LOG(ERROR) << "Unable to create directory " << directory;
    return SP_NOTOK;
  }
  return SP_OK;
}

int LocalFS::createTmpCkptFile(const Checkpoint& _ckpt) {
  auto code = ::open(tempCkptFile(_ckpt).c_str(), O_CREAT | O_WRONLY, 0644);
  if (code < 0) {
    PLOG(ERROR) << "Unable to create temporary checkpoint file " << tempCkptFile(_ckpt);
    return SP_NOTOK;
  }
  return code;
}

int LocalFS::writeTmpCkptFile(int fd, const Checkpoint& _ckpt) {
  size_t len = _ckpt.nbytes();
  char* buf = reinterpret_cast<char*>(_ckpt.checkpoint());

  if (!FileUtils::writeAll(fd, buf, len)) {
    PLOG(ERROR) << "Unable to write to temporary checkpoint file " << tempCkptFile(_ckpt);
    return SP_NOTOK;
  }

  return SP_OK;
}

int LocalFS::closeTmpCkptFile(int fd, const Checkpoint& _ckpt) {
  // force flush the file contents to persistent store
  auto code = ::fsync(fd);
  if (code < 0) {
    PLOG(ERROR) << "Unable to sync temporary checkpoint file " << tempCkptFile(_ckpt);
    return SP_NOTOK;
  }

  // close the file descriptor
  code = ::close(fd);
  if (code < 0) {
    PLOG(ERROR) << "Unable to close temporary checkpoint file " << tempCkptFile(_ckpt);
    return SP_NOTOK;
  }
  return SP_OK;
}

int LocalFS::moveTmpCkptFile(const Checkpoint& _ckpt) {
  auto code = ::rename(tempCkptFile(_ckpt).c_str(), ckptFile(_ckpt).c_str());
  if (code < 0) {
    PLOG(ERROR) << "Unable to move temporary checkpoint file " << tempCkptFile(_ckpt);
    return SP_NOTOK;
  }
  return SP_OK;
}

int LocalFS::store(const Checkpoint& _ckpt) {
  // create the checkpoint directory, if not there
  if (createCkptDirectory(_ckpt) == SP_NOTOK) {
    LOG(ERROR) << "Checkpoint failed for " << logMessageFragment(_ckpt);
    return SP_NOTOK;
  }
  LOG(INFO) << "Created checkpoint directory " << ckptDirectory(_ckpt);

  // create and open the temporary checkpoint file
  auto fd = createTmpCkptFile(_ckpt);
  if (fd == SP_NOTOK) {
    LOG(ERROR) << "Checkpoint failed for " << logMessageFragment(_ckpt);
    return SP_NOTOK;
  }
  LOG(INFO) << "Created temp checkpoint file " << tempCkptFile(_ckpt);

  // write the protobuf into the temporary checkpoint file
  if (writeTmpCkptFile(fd, _ckpt) == SP_NOTOK) {
    LOG(ERROR) << "Checkpoint failed for " << logMessageFragment(_ckpt);
    return SP_NOTOK;
  }
  LOG(INFO) << "Write temp checkpoint file " << tempCkptFile(_ckpt);

  // close the temporary checkpoint file
  if (!FileUtils::closeSync(fd)) {
    LOG(ERROR) << "Checkpoint failed for " << logMessageFragment(_ckpt);
    return SP_NOTOK;
  }
  LOG(INFO) << "Closed temp checkpoint file " << tempCkptFile(_ckpt);

  // move the temporary checkpoint file to final destination
  if (!FileUtils::rename(tempCkptFile(_ckpt).c_str(), ckptFile(_ckpt).c_str())) {
    LOG(ERROR) << "Checkpoint failed for " << logMessageFragment(_ckpt);
    return SP_NOTOK;
  }
  LOG(INFO) << "Moved temp checkpoint file " << tempCkptFile(_ckpt) << " "
            << "to " << ckptFile(_ckpt);
  LOG(INFO) << "Checkpoint successful for " << logMessageFragment(_ckpt);

  return SP_OK;
}

int LocalFS::restore(Checkpoint& _ckpt) {
  std::string file = ckptFile(_ckpt);

  // open the checkpoint file
  std::ifstream ifile(ckptFile(_ckpt), std::ifstream::in | std::ifstream::binary);
  if (!ifile.is_open()) {
    PLOG(ERROR) << "Unable to open checkpoint file " << tempCkptFile(_ckpt);
    LOG(ERROR) << "Restore checkpoint failed for " << logMessageFragment(_ckpt);
    return SP_NOTOK;
  }

  // read the protobuf from checkpoint file
  auto savedbytes = new ::heron::proto::ckptmgr::SaveInstanceStateRequest;
  if (!savedbytes->ParseFromIstream(&ifile)) {
    LOG(ERROR) << "Restore checkpoint failed for " << logMessageFragment(_ckpt);
    return SP_NOTOK;
  }

  // pass the retrieved bytes to checkpoint
  _ckpt.set_checkpoint(savedbytes);

  // close the checkpoint file
  ifile.close();
  return SP_OK;
}

}  // namespace ckptmgr
}  // namespace heron

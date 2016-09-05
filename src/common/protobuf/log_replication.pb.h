// Generated by the protocol buffer compiler.  DO NOT EDIT!
// source: log_replication.proto

#ifndef PROTOBUF_log_5freplication_2eproto__INCLUDED
#define PROTOBUF_log_5freplication_2eproto__INCLUDED

#include <string>

#include <google/protobuf/stubs/common.h>

#if GOOGLE_PROTOBUF_VERSION < 2004000
#error This file was generated by a newer version of protoc which is
#error incompatible with your Protocol Buffer headers.  Please update
#error your headers.
#endif
#if 2004001 < GOOGLE_PROTOBUF_MIN_PROTOC_VERSION
#error This file was generated by an older version of protoc which is
#error incompatible with your Protocol Buffer headers.  Please
#error regenerate this file with a newer version of protoc.
#endif

#include <google/protobuf/generated_message_util.h>
#include <google/protobuf/repeated_field.h>
#include <google/protobuf/extension_set.h>
#include <google/protobuf/generated_message_reflection.h>
// @@protoc_insertion_point(includes)

namespace replication {

// Internal implementation detail -- do not call these.
void  protobuf_AddDesc_log_5freplication_2eproto();
void protobuf_AssignDesc_log_5freplication_2eproto();
void protobuf_ShutdownFile_log_5freplication_2eproto();

class Replication;

// ===================================================================

class Replication : public ::google::protobuf::Message {
 public:
  Replication();
  virtual ~Replication();
  
  Replication(const Replication& from);
  
  inline Replication& operator=(const Replication& from) {
    CopyFrom(from);
    return *this;
  }
  
  inline const ::google::protobuf::UnknownFieldSet& unknown_fields() const {
    return _unknown_fields_;
  }
  
  inline ::google::protobuf::UnknownFieldSet* mutable_unknown_fields() {
    return &_unknown_fields_;
  }
  
  static const ::google::protobuf::Descriptor* descriptor();
  static const Replication& default_instance();
  
  void Swap(Replication* other);
  
  // implements Message ----------------------------------------------
  
  Replication* New() const;
  void CopyFrom(const ::google::protobuf::Message& from);
  void MergeFrom(const ::google::protobuf::Message& from);
  void CopyFrom(const Replication& from);
  void MergeFrom(const Replication& from);
  void Clear();
  bool IsInitialized() const;
  
  int ByteSize() const;
  bool MergePartialFromCodedStream(
      ::google::protobuf::io::CodedInputStream* input);
  void SerializeWithCachedSizes(
      ::google::protobuf::io::CodedOutputStream* output) const;
  ::google::protobuf::uint8* SerializeWithCachedSizesToArray(::google::protobuf::uint8* output) const;
  int GetCachedSize() const { return _cached_size_; }
  private:
  void SharedCtor();
  void SharedDtor();
  void SetCachedSize(int size) const;
  public:
  
  ::google::protobuf::Metadata GetMetadata() const;
  
  // nested types ----------------------------------------------------
  
  // accessors -------------------------------------------------------
  
  // required int32 fileID = 1;
  inline bool has_fileid() const;
  inline void clear_fileid();
  static const int kFileIDFieldNumber = 1;
  inline ::google::protobuf::int32 fileid() const;
  inline void set_fileid(::google::protobuf::int32 value);
  
  // required int64 fileoffset = 2;
  inline bool has_fileoffset() const;
  inline void clear_fileoffset();
  static const int kFileoffsetFieldNumber = 2;
  inline ::google::protobuf::int64 fileoffset() const;
  inline void set_fileoffset(::google::protobuf::int64 value);
  
  // required int64 data_size = 3;
  inline bool has_data_size() const;
  inline void clear_data_size();
  static const int kDataSizeFieldNumber = 3;
  inline ::google::protobuf::int64 data_size() const;
  inline void set_data_size(::google::protobuf::int64 value);
  
  // required int64 checksum = 4;
  inline bool has_checksum() const;
  inline void clear_checksum();
  static const int kChecksumFieldNumber = 4;
  inline ::google::protobuf::int64 checksum() const;
  inline void set_checksum(::google::protobuf::int64 value);
  
  // required bytes log_data = 5;
  inline bool has_log_data() const;
  inline void clear_log_data();
  static const int kLogDataFieldNumber = 5;
  inline const ::std::string& log_data() const;
  inline void set_log_data(const ::std::string& value);
  inline void set_log_data(const char* value);
  inline void set_log_data(const void* value, size_t size);
  inline ::std::string* mutable_log_data();
  inline ::std::string* release_log_data();
  
  // @@protoc_insertion_point(class_scope:replication.Replication)
 private:
  inline void set_has_fileid();
  inline void clear_has_fileid();
  inline void set_has_fileoffset();
  inline void clear_has_fileoffset();
  inline void set_has_data_size();
  inline void clear_has_data_size();
  inline void set_has_checksum();
  inline void clear_has_checksum();
  inline void set_has_log_data();
  inline void clear_has_log_data();
  
  ::google::protobuf::UnknownFieldSet _unknown_fields_;
  
  ::google::protobuf::int64 fileoffset_;
  ::google::protobuf::int64 data_size_;
  ::google::protobuf::int64 checksum_;
  ::std::string* log_data_;
  ::google::protobuf::int32 fileid_;
  
  mutable int _cached_size_;
  ::google::protobuf::uint32 _has_bits_[(5 + 31) / 32];
  
  friend void  protobuf_AddDesc_log_5freplication_2eproto();
  friend void protobuf_AssignDesc_log_5freplication_2eproto();
  friend void protobuf_ShutdownFile_log_5freplication_2eproto();
  
  void InitAsDefaultInstance();
  static Replication* default_instance_;
};
// ===================================================================


// ===================================================================

// Replication

// required int32 fileID = 1;
inline bool Replication::has_fileid() const {
  return (_has_bits_[0] & 0x00000001u) != 0;
}
inline void Replication::set_has_fileid() {
  _has_bits_[0] |= 0x00000001u;
}
inline void Replication::clear_has_fileid() {
  _has_bits_[0] &= ~0x00000001u;
}
inline void Replication::clear_fileid() {
  fileid_ = 0;
  clear_has_fileid();
}
inline ::google::protobuf::int32 Replication::fileid() const {
  return fileid_;
}
inline void Replication::set_fileid(::google::protobuf::int32 value) {
  set_has_fileid();
  fileid_ = value;
}

// required int64 fileoffset = 2;
inline bool Replication::has_fileoffset() const {
  return (_has_bits_[0] & 0x00000002u) != 0;
}
inline void Replication::set_has_fileoffset() {
  _has_bits_[0] |= 0x00000002u;
}
inline void Replication::clear_has_fileoffset() {
  _has_bits_[0] &= ~0x00000002u;
}
inline void Replication::clear_fileoffset() {
  fileoffset_ = GOOGLE_LONGLONG(0);
  clear_has_fileoffset();
}
inline ::google::protobuf::int64 Replication::fileoffset() const {
  return fileoffset_;
}
inline void Replication::set_fileoffset(::google::protobuf::int64 value) {
  set_has_fileoffset();
  fileoffset_ = value;
}

// required int64 data_size = 3;
inline bool Replication::has_data_size() const {
  return (_has_bits_[0] & 0x00000004u) != 0;
}
inline void Replication::set_has_data_size() {
  _has_bits_[0] |= 0x00000004u;
}
inline void Replication::clear_has_data_size() {
  _has_bits_[0] &= ~0x00000004u;
}
inline void Replication::clear_data_size() {
  data_size_ = GOOGLE_LONGLONG(0);
  clear_has_data_size();
}
inline ::google::protobuf::int64 Replication::data_size() const {
  return data_size_;
}
inline void Replication::set_data_size(::google::protobuf::int64 value) {
  set_has_data_size();
  data_size_ = value;
}

// required int64 checksum = 4;
inline bool Replication::has_checksum() const {
  return (_has_bits_[0] & 0x00000008u) != 0;
}
inline void Replication::set_has_checksum() {
  _has_bits_[0] |= 0x00000008u;
}
inline void Replication::clear_has_checksum() {
  _has_bits_[0] &= ~0x00000008u;
}
inline void Replication::clear_checksum() {
  checksum_ = GOOGLE_LONGLONG(0);
  clear_has_checksum();
}
inline ::google::protobuf::int64 Replication::checksum() const {
  return checksum_;
}
inline void Replication::set_checksum(::google::protobuf::int64 value) {
  set_has_checksum();
  checksum_ = value;
}

// required bytes log_data = 5;
inline bool Replication::has_log_data() const {
  return (_has_bits_[0] & 0x00000010u) != 0;
}
inline void Replication::set_has_log_data() {
  _has_bits_[0] |= 0x00000010u;
}
inline void Replication::clear_has_log_data() {
  _has_bits_[0] &= ~0x00000010u;
}
inline void Replication::clear_log_data() {
  if (log_data_ != &::google::protobuf::internal::kEmptyString) {
    log_data_->clear();
  }
  clear_has_log_data();
}
inline const ::std::string& Replication::log_data() const {
  return *log_data_;
}
inline void Replication::set_log_data(const ::std::string& value) {
  set_has_log_data();
  if (log_data_ == &::google::protobuf::internal::kEmptyString) {
    log_data_ = new ::std::string;
  }
  log_data_->assign(value);
}
inline void Replication::set_log_data(const char* value) {
  set_has_log_data();
  if (log_data_ == &::google::protobuf::internal::kEmptyString) {
    log_data_ = new ::std::string;
  }
  log_data_->assign(value);
}
inline void Replication::set_log_data(const void* value, size_t size) {
  set_has_log_data();
  if (log_data_ == &::google::protobuf::internal::kEmptyString) {
    log_data_ = new ::std::string;
  }
  log_data_->assign(reinterpret_cast<const char*>(value), size);
}
inline ::std::string* Replication::mutable_log_data() {
  set_has_log_data();
  if (log_data_ == &::google::protobuf::internal::kEmptyString) {
    log_data_ = new ::std::string;
  }
  return log_data_;
}
inline ::std::string* Replication::release_log_data() {
  clear_has_log_data();
  if (log_data_ == &::google::protobuf::internal::kEmptyString) {
    return NULL;
  } else {
    ::std::string* temp = log_data_;
    log_data_ = const_cast< ::std::string*>(&::google::protobuf::internal::kEmptyString);
    return temp;
  }
}


// @@protoc_insertion_point(namespace_scope)

}  // namespace replication

#ifndef SWIG
namespace google {
namespace protobuf {


}  // namespace google
}  // namespace protobuf
#endif  // SWIG

// @@protoc_insertion_point(global_scope)

#endif  // PROTOBUF_log_5freplication_2eproto__INCLUDED

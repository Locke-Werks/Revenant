// The Whisper model on disk, and its download on first use.
//
// WHISPER'S WEIGHTS ARE THE PROJECT'S FIRST MODEL-FILE DEPENDENCY, accepted by
// the owner on 2026-10-03. They are 1.6 GB and are not in the installer: the
// first transcription request fetches them into the user's own profile, where
// nothing else on the machine depends on them and no other project's model
// folder is read.
//
// THE BYTES ARE PINNED THREE WAYS. The URL names a commit of the Hugging Face
// repository rather than its main branch, so the file it serves cannot be
// replaced under the same address. The size and the SHA-256 come from that
// commit's Git LFS pointer. A download that does not match both is deleted
// rather than kept.
//
// HASHED ONCE. Verifying 1.6 GB takes seconds, which is not a cost to pay on
// every engine start. A verified file gets a sidecar beside it holding the
// hash it was verified against, its size and its modification time, and
// model_ready() trusts the sidecar while all three still describe the file.
// Anything that rewrites the file moves its modification time, and the next
// check hashes it again.
//
// WinHTTP and BCrypt, both in Windows. No new library for either.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "core/error.h"

namespace revenant::transcribe {

struct ModelSpec {
    // The file name on disk, and the name in the repository.
    std::string_view name;
    // https only. Redirects are followed, to https only.
    std::string_view url;
    // Lowercase hexadecimal.
    std::string_view sha256;
    std::uint64_t size = 0;
};

// Whisper large-v3-turbo, in whisper.cpp's ggml format, from
// huggingface.co/ggerganov/whisper.cpp at commit 5359861c, which was the
// head of main on 2026-10-03 (its X-Repo-Commit header that day). The size
// and hash are the X-Linked-Size and X-Linked-ETag the same request returned,
// which are the LFS pointer's.
//
// large-v3-turbo is large-v3's encoder with a four-layer decoder in place of
// thirty-two. The decoder is what runs once per token, so it decodes several
// times faster than large-v3 for a small loss in accuracy, and its encoder is
// still the large one that copes with band-limited, noisy speech.
inline constexpr ModelSpec kDefaultModel{
    "ggml-large-v3-turbo.bin",
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/"
    "ggml-large-v3-turbo.bin",
    "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69",
    1'624'555'275,
};

// %LOCALAPPDATA%\Revenant\models, created if it is not there. Local rather
// than roaming: 1.6 GB has no business following a profile around a domain.
// When the directory cannot be created the path is still returned, and the
// first operation that needs it fails saying why.
[[nodiscard]] std::filesystem::path model_directory();

// Where a model lives: model_directory() / spec.name, or directory / spec.name.
[[nodiscard]] std::filesystem::path model_path(const ModelSpec& spec);
[[nodiscard]] std::filesystem::path model_path(const ModelSpec& spec,
                                               const std::filesystem::path& directory);

// The sidecar that records a verification, beside the model.
[[nodiscard]] std::filesystem::path verified_path(const std::filesystem::path& model);

// True when the model is present, the size the spec says, and verified: by a
// sidecar that still describes the file, or by hashing it now and writing the
// sidecar when that matches. False for absent, short, long or wrong; a wrong
// file is left where it is for download_model to replace. An error only for a
// failure to read, which is not the same answer as "absent".
[[nodiscard]] Expected<bool> model_ready(const ModelSpec& spec);
[[nodiscard]] Expected<bool> model_ready(const ModelSpec& spec, const std::filesystem::path& directory);

struct DownloadProgress {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
};

// Fetches the model into its directory, blocking the caller's thread.
//
// Streams to <name>.part in the same directory and resumes a .part left by an
// earlier attempt with a Range request. Hashes as bytes arrive. Refuses, and
// deletes the .part, on a size or hash mismatch. Renames into place only once
// both match, so a model file that exists is always a whole one, and writes
// the sidecar. Nothing to do when model_ready() already says true.
//
// progress is called as bytes arrive; returning false cancels, which keeps the
// .part for the next attempt to resume and returns an error saying it was
// cancelled. It may be empty.
[[nodiscard]] Status download_model(const ModelSpec& spec,
                                    const std::function<bool(const DownloadProgress&)>& progress);
[[nodiscard]] Status download_model(const ModelSpec& spec,
                                    const std::function<bool(const DownloadProgress&)>& progress,
                                    const std::filesystem::path& directory);

// The SHA-256 of a file, lowercase hexadecimal. What model_ready and
// download_model verify with, exposed for the tests and for a person checking
// a file by hand.
[[nodiscard]] Expected<std::string> sha256_file(const std::filesystem::path& path);

}  // namespace revenant::transcribe

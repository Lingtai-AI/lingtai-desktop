#!/usr/bin/env bash
# Fetch only the exact source and resource inputs declared in the JSON lock.
# Existing inputs are verification-only: a wrong or dirty checkout is rejected.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
LOCK="$ROOT/cmake/desktop-app-toolkit-lock.json"
DEPS_ROOT="$ROOT/.deps"
LOCAL_TOOLKIT_ROOT=""
LOCAL_THIRD_PARTY_ROOT=""

usage() {
  cat <<USAGE
Usage: $0 [--deps-root PATH] [--local-toolkit-root PATH] [--local-third-party-root PATH]

Fetch exact lock-manifest inputs into the ignored dependency root. Local roots
are optional generic sources for selected, independently obtained exact Git
checkouts; they are verified before clone and never patched in place.
USAGE
}

die() {
  printf 'bootstrap-deps: %s\n' "$*" >&2
  exit 1
}

while (($#)); do
  case "$1" in
    --deps-root)
      (($# >= 2)) || die "--deps-root needs a path"
      DEPS_ROOT="$2"
      shift 2
      ;;
    --local-toolkit-root)
      (($# >= 2)) || die "--local-toolkit-root needs a path"
      LOCAL_TOOLKIT_ROOT="$2"
      shift 2
      ;;
    --local-third-party-root)
      (($# >= 2)) || die "--local-third-party-root needs a path"
      LOCAL_THIRD_PARTY_ROOT="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *) die "unknown option: $1" ;;
  esac
done

for command in git curl python3; do
  command -v "$command" >/dev/null || die "required command not found: $command"
done
[[ -f "$LOCK" ]] || die "missing lock manifest: $LOCK"

sha256_file() {
  if command -v shasum >/dev/null; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null; then
    sha256sum "$1" | awk '{print $1}'
  else
    die "required checksum command not found (shasum or sha256sum)"
  fi
}

verify_git_checkout() {
  local directory="$1" url="$2" commit="$3" label="$4"
  [[ -e "$directory/.git" ]] || die "$label exists but is not a Git checkout: $directory"
  local actual remote changes
  actual="$(git -C "$directory" rev-parse HEAD 2>/dev/null)" \
    || die "$label is not a readable Git checkout: $directory"
  remote="$(git -C "$directory" remote get-url origin 2>/dev/null)" \
    || die "$label has no origin remote: $directory"
  changes="$(git -C "$directory" status --porcelain --untracked-files=all)"
  [[ "$remote" == "$url" ]] || die "$label has unexpected origin: $remote"
  [[ "$actual" == "$commit" ]] || die "$label is at $actual, expected $commit"
  [[ -z "$changes" ]] || die "$label has local changes: $directory"
}

clone_local_checkout() {
  local source="$1" destination="$2" url="$3" commit="$4" label="$5"
  verify_git_checkout "$source" "$url" "$commit" "local $label"
  git clone --quiet --no-local --no-checkout "$source" "$destination"
  git -C "$destination" remote set-url origin "$url"
  git -C "$destination" checkout --quiet --detach "$commit"
  verify_git_checkout "$destination" "$url" "$commit" "$label"
}

fetch_git_checkout() {
  local destination="$1" url="$2" commit="$3" label="$4" local_source="$5"
  if [[ -e "$destination" ]]; then
    verify_git_checkout "$destination" "$url" "$commit" "$label"
    return
  fi
  mkdir -p "$(dirname "$destination")"
  if [[ -n "$local_source" ]]; then
    [[ -d "$local_source" ]] || die "local source is absent: $local_source"
    clone_local_checkout "$local_source" "$destination" "$url" "$commit" "$label"
  else
    git init --quiet "$destination"
    git -C "$destination" remote add origin "$url"
    git -C "$destination" fetch --quiet --no-tags --depth 1 origin "$commit"
    git -C "$destination" checkout --quiet --detach FETCH_HEAD
    verify_git_checkout "$destination" "$url" "$commit" "$label"
  fi
}

fetch_raw_file() {
  local destination="$1" url="$2" expected_sha256="$3" label="$4"
  if [[ -e "$destination" ]]; then
    [[ -f "$destination" ]] || die "$label exists but is not a regular file: $destination"
  else
    mkdir -p "$(dirname "$destination")"
    local temporary
    temporary="$(mktemp "${destination}.tmp.XXXXXX")"
    trap 'rm -f "$temporary"' RETURN
    curl --fail --location --silent --show-error "$url" -o "$temporary"
    mv "$temporary" "$destination"
    trap - RETURN
  fi
  [[ "$(sha256_file "$destination")" == "$expected_sha256" ]] \
    || die "$label checksum mismatch: $destination"
}

fetch_resource_blob() {
  local destination="$1" url="$2" expected_blob="$3" label="$4"
  if [[ -e "$destination" ]]; then
    [[ -f "$destination" ]] || die "$label exists but is not a regular file: $destination"
  else
    mkdir -p "$(dirname "$destination")"
    local temporary
    temporary="$(mktemp "${destination}.tmp.XXXXXX")"
    trap 'rm -f "$temporary"' RETURN
    curl --fail --location --silent --show-error "$url" -o "$temporary"
    mv "$temporary" "$destination"
    trap - RETURN
  fi
  [[ "$(git hash-object "$destination")" == "$expected_blob" ]] \
    || die "$label Git blob checksum mismatch: $destination"
}

emit_lock_rows() {
  python3 - "$LOCK" <<'PY'
import json
import sys

lock = json.load(open(sys.argv[1], encoding="utf-8"))
for source in lock["sources"]:
    kind = source["kind"]
    if kind == "git":
        print("\t".join(("git", source["name"], source["destination"], source["url"], source["commit"])))
    elif kind == "raw-files":
        for item in source["files"]:
            print("\t".join(("raw", source["name"], source["destination"], source["url"], item["path"], item["sha256"])))
    else:
        raise SystemExit(f"unsupported source kind: {kind}")
for resource in lock["resources"]:
    print("\t".join(("resource", resource["path"], resource["url"], resource["git_blob_sha1"])))
PY
}

mkdir -p "$DEPS_ROOT"
lock_rows="$(emit_lock_rows)"
while IFS=$'\t' read -r kind one two three four five; do
  [[ -n "$kind" ]] || continue
  case "$kind" in
    git)
      name="$one"
      destination="$DEPS_ROOT/$two"
      url="$three"
      commit="$four"
      local_source=""
      case "$two" in
        src/*)
          [[ -z "$LOCAL_TOOLKIT_ROOT" ]] || local_source="$LOCAL_TOOLKIT_ROOT/$name"
          ;;
        third_party/*)
          [[ -z "$LOCAL_THIRD_PARTY_ROOT" ]] || local_source="$LOCAL_THIRD_PARTY_ROOT/$name"
          ;;
        *) die "unsupported Git destination in lock: $two" ;;
      esac
      fetch_git_checkout "$destination" "$url" "$commit" "$name" "$local_source"
      ;;
    raw)
      fetch_raw_file "$DEPS_ROOT/$two/$four" "$three/$four" "$five" "$one/$four"
      ;;
    resource)
      fetch_resource_blob "$DEPS_ROOT/tdesktop-resources/$one" "$two" "$three" "$one"
      ;;
    *) die "unsupported lock row kind: $kind" ;;
  esac
done <<<"$lock_rows"

build_openssl() {
  [[ "$(uname -s)" == "Darwin" ]] || die "the locked universal OpenSSL build currently requires macOS"
  command -v make >/dev/null || die "required command not found: make"
  command -v lipo >/dev/null || die "required command not found: lipo"

  local source="$DEPS_ROOT/third_party/openssl"
  local output="$DEPS_ROOT/build/openssl"
  local jobs="${BUILD_JOBS:-8}"
  local expected="a7e992847de83aa36be0c399c89db3fb827b0be2"

  if [[ -e "$output" ]]; then
    [[ -f "$output/lib/libssl.a" && -f "$output/lib/libcrypto.a" && -d "$output/include/openssl" ]] \
      || die "existing OpenSSL output is incomplete: $output"
    local architecture
    architecture="$(lipo -info "$output/lib/libssl.a")"
    [[ "$architecture" == *arm64* && "$architecture" == *x86_64* ]] \
      || die "existing OpenSSL libssl.a is not universal: $architecture"
    architecture="$(lipo -info "$output/lib/libcrypto.a")"
    [[ "$architecture" == *arm64* && "$architecture" == *x86_64* ]] \
      || die "existing OpenSSL libcrypto.a is not universal: $architecture"
    return
  fi

  verify_git_checkout "$source" "https://github.com/openssl/openssl.git" "$expected" "openssl"
  mkdir -p "$output/lib" "$output/include"
  (
    local completed=0
    cleanup() {
      git -C "$source" reset --hard --quiet "$expected"
      git -C "$source" clean -fdx --quiet
      if [[ "$completed" != 1 ]]; then
        rm -rf "$output"
      fi
    }
    trap cleanup EXIT
    cd "$source"
    ./Configure darwin64-arm64-cc no-shared no-tests -mmacosx-version-min=10.13
    make -j"$jobs" build_libs
    cp libcrypto.a "$output/lib/libcrypto.arm64.a"
    cp libssl.a "$output/lib/libssl.arm64.a"
    make clean
    git clean -fdx --quiet
    ./Configure darwin64-x86_64-cc no-shared no-tests -mmacosx-version-min=10.13
    make -j"$jobs" build_libs
    cp libcrypto.a "$output/lib/libcrypto.x86_64.a"
    cp libssl.a "$output/lib/libssl.x86_64.a"
    cp -R include/openssl "$output/include/"
    lipo -create "$output/lib/libcrypto.arm64.a" "$output/lib/libcrypto.x86_64.a" -output "$output/lib/libcrypto.a"
    lipo -create "$output/lib/libssl.arm64.a" "$output/lib/libssl.x86_64.a" -output "$output/lib/libssl.a"
    rm "$output/lib/libcrypto.arm64.a" "$output/lib/libcrypto.x86_64.a" \
      "$output/lib/libssl.arm64.a" "$output/lib/libssl.x86_64.a"
    completed=1
  )
  build_openssl
}

prepare_parent_inputs() {
  local parent="$DEPS_ROOT/parent-inputs"
  mkdir -p "$parent"
  local link expected
  for link in lib_ui Resources; do
    expected="../src/lib_ui"
    [[ "$link" == "Resources" ]] && expected="../tdesktop-resources/Resources"
    if [[ -e "$parent/$link" || -L "$parent/$link" ]]; then
      [[ -L "$parent/$link" ]] || die "parent input is not the expected symlink: $parent/$link"
      [[ "$(readlink "$parent/$link")" == "$expected" ]] \
        || die "parent input symlink has unexpected destination: $parent/$link"
    else
      ln -s "$expected" "$parent/$link"
    fi
    [[ -e "$parent/$link" ]] || die "parent input symlink is dangling: $parent/$link"
  done
}

prepare_parent_inputs
build_openssl
printf 'bootstrap-deps: verified lock inputs in %s\n' "$DEPS_ROOT"

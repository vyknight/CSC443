// tombstone.h
#pragma once
#include <string>

// reserved value，express tombstone(delete notation)
// it's not safe to use in the real life, but it's ok in this project
// since in document it allow us assume the input can be always 8-byte integers
static const std::string KV_TOMBSTONE = "\x01";

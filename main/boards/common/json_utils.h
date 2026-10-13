#pragma once

#include <string>

/** Escape a string for embedding inside a JSON string literal. */
std::string EscapeJsonString(const std::string& input);

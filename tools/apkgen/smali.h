#pragma once

#include <string>
#include <string_view>

#include "dex_writer.h"

namespace aa::tool {

// Assembles one smali source file into a class definition. Supported subset:
// .class/.super/.implements/.source/.field (with initial values)/.method,
// .registers/.locals, every Dalvik instruction except invoke-polymorphic,
// invoke-custom and method handles, labels, .catch/.catchall,
// .packed-switch/.sparse-switch/.array-data. Debug directives (.line, .param,
// .local, ...) and annotations are accepted and ignored.
// Throws std::runtime_error("<file>:<line> : message") on error.
ClassDef assemble_smali(std::string_view source, const std::string& file_name);

}  // namespace aa::tool

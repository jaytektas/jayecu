// PresetOptions::compileFor — the one place a preset's expression SOURCE becomes bytecode.
//
// Out of line because it needs the compiler and the meta, and PresetOptions.h is included by the
// widget layer: a preset is a list of writes, and only this half of it knows what a program is.

#include "PresetOptions.h"
#include "ExprCompiler.h"
#include "MetaModel.h"
#include "MathEvaluator.h"

std::vector<uint8_t> PresetOptions::compileFor(const std::string& path, const std::string& source) {
    const Cache& c = Cache::instance();
    const MetaModel* meta = c.meta();
    if (!meta) return {};
    // The FIELD's capacity, not the VM's maximum: a program that fits the compiler's limit but not
    // this slot's block would be truncated on the way in, which is a program that means something
    // else. resolveBlob is also the check that the path really is a program field.
    int off = 0, size = 0;
    if (!meta->resolveBlob(path, off, size) || size <= 0) return {};
    ExprCompiler::Result r = ExprCompiler::compile(source, *meta,
                                                   static_cast<uint32_t>(c.configImage().size()),
                                                   static_cast<uint16_t>(size));
    return r.ok ? r.code : std::vector<uint8_t>{};
}

// See the declaration: a preset may address "whichever element the page is showing".
std::string PresetOptions::resolved(const std::string& path) {
    return (path.find("[@") == std::string::npos) ? path
                                                  : MathEvaluator::instance().resolveIndexed(path);
}

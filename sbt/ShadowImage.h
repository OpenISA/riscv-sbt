#ifndef SBT_SHADOWIMAGE_H
#define SBT_SHADOWIMAGE_H

#include "Context.h"

#include <map>

namespace llvm {
class Constant;
}

namespace sbt {

class Object;

class ShadowImage
{
public:
    ShadowImage(Context* ctx, const Object* obj);

    llvm::Constant* getSection(const std::string& name) const
    {
        auto it = _sections.find(name);
        xassert(it != _sections.end() && "Section not found in ShadowImage!");
        return it->second;
    }

private:
    void build();

    Context* _ctx;
    const Object* _obj;
    std::map<std::string, llvm::Constant*> _sections;
};

}

#endif
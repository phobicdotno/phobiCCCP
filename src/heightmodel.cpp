#include "heightmodel.h"

namespace c2d {

static HeightModelProvider &providerRef()
{
    static HeightModelProvider p;
    return p;
}

void setHeightModelProvider(HeightModelProvider provider)
{
    providerRef() = std::move(provider);
}

const HeightModel *heightModelFor(const Document &doc)
{
    const HeightModelProvider &p = providerRef();
    return p ? p(doc) : nullptr;
}

} // namespace c2d

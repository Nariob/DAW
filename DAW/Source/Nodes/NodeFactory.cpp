#include "NodeFactory.h"

#include <algorithm>

namespace daw {

NodeFactory& NodeFactory::get()
{
    static NodeFactory instance;
    return instance;
}

NodeFactory::NodeFactory()
{
    registerBuiltinNodes (*this);
}

void NodeFactory::registerType (NodeTypeInfo info)
{
    for (auto& t : types)
        if (t.typeId == info.typeId) { t = std::move (info); return; }
    types.push_back (std::move (info));
}

const NodeTypeInfo* NodeFactory::find (const std::string& typeId) const
{
    for (auto& t : types)
        if (t.typeId == typeId)
            return &t;
    return nullptr;
}

std::unique_ptr<Node> NodeFactory::create (const std::string& typeId) const
{
    if (auto* info = find (typeId))
        if (info->create)
            return info->create();
    return nullptr;
}

std::vector<std::string> NodeFactory::getCategories() const
{
    std::vector<std::string> cats;
    for (auto& t : types)
        if (t.userCreatable && std::find (cats.begin(), cats.end(), t.category) == cats.end())
            cats.push_back (t.category);
    return cats;
}

} // namespace daw

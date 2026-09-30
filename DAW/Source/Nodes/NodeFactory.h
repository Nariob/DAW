#pragma once
// =============================================================================
//  NodeFactory.h  -  Registro de todos los tipos de nodo disponibles.
//  La UI usa getTypes() para construir el menu "Agregar nodo".
// =============================================================================
#include "Node.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace daw {

struct NodeTypeInfo
{
    std::string typeId;
    std::string displayName;
    std::string category;
    std::string description;
    std::function<std::unique_ptr<Node>()> create;
    bool userCreatable = true;
};

class NodeFactory
{
public:
    static NodeFactory& get();

    void registerType (NodeTypeInfo info);
    std::unique_ptr<Node> create (const std::string& typeId) const;

    const NodeTypeInfo* find (const std::string& typeId) const;
    const std::vector<NodeTypeInfo>& getTypes() const { return types; }
    std::vector<std::string> getCategories() const;

private:
    NodeFactory();
    std::vector<NodeTypeInfo> types;
};

// Definida en BuiltinNodes.cpp
void registerBuiltinNodes (NodeFactory& factory);

} // namespace daw

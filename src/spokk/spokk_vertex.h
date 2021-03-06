#pragma once

#include "spokk_mesh.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace spokk {

struct VertexLayout {
  struct AttributeInfo {
    uint32_t location;
    VkFormat format;
    uint32_t offset;
  };

  // Assumes vertices are tightly packed; stride will be the highest attribute
  // offset plus that attribute's size.
  VertexLayout(std::initializer_list<AttributeInfo> attr_infos);
  // Another shortcut to build a VertexLayout from a MeshFormat.
  // NOTE: binding is the bind point of the buffer to use, *not* the index of its description
  // in the vertex_buffer_bindings array!
  VertexLayout(const MeshFormat &mesh_format, uint32_t binding);

  uint32_t stride;
  std::vector<AttributeInfo> attributes;
};

// Converts all attributes in src to the corresponding format in dst.
// Attributes are matched by their "location" values.
// Only attributes present in both layouts will be processed.
// Returns 0 on success, non-zero on errors.
int ConvertVertexBuffer(const void *src_vertices, const VertexLayout &src_layout, void *dst_vertices,
    const VertexLayout &dst_layout, size_t vertex_count);

}  // namespace spokk

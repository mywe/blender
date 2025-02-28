/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * Engine for drawing a selection map where the pixels indicate the selection indices.
 */

#include "BKE_editmesh.hh"
#include "BKE_mesh.hh"

#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "ED_view3d.hh"

#include "DEG_depsgraph_query.hh"

#include "draw_cache_impl.hh"

#include "select_private.hh"

/* -------------------------------------------------------------------- */
/** \name Draw Utilities
 * \{ */

short select_id_get_object_select_mode(Scene *scene, Object *ob)
{
  short r_select_mode = 0;
  if (ob->mode & (OB_MODE_WEIGHT_PAINT | OB_MODE_VERTEX_PAINT | OB_MODE_TEXTURE_PAINT)) {
    /* In order to sample flat colors for vertex weights / texture-paint / vertex-paint
     * we need to be in SCE_SELECT_FACE mode so select_cache_init() correctly sets up
     * a shgroup with select_id_flat.
     * Note this is not working correctly for vertex-paint (yet), but has been discussed
     * in #66645 and there is a solution by @mano-wii in P1032.
     * So OB_MODE_VERTEX_PAINT is already included here [required for P1032 I guess]. */
    Mesh *me_orig = static_cast<Mesh *>(DEG_get_original_object(ob)->data);
    if (me_orig->editflag & ME_EDIT_PAINT_VERT_SEL) {
      r_select_mode = SCE_SELECT_VERTEX;
    }
    else {
      r_select_mode = SCE_SELECT_FACE;
    }
  }
  else {
    r_select_mode = scene->toolsettings->selectmode;
  }

  return r_select_mode;
}

static bool check_ob_drawface_dot(short select_mode, const View3D *v3d, eDrawType dt)
{
  if (select_mode & SCE_SELECT_FACE) {
    if ((dt < OB_SOLID) || XRAY_FLAG_ENABLED(v3d)) {
      return true;
    }
    if (v3d->overlay.edit_flag & V3D_OVERLAY_EDIT_FACE_DOT) {
      return true;
    }
  }
  return false;
}

static void draw_select_id_edit_mesh(SELECTID_Instance &inst,
                                     Object *ob,
                                     ResourceHandle res_handle,
                                     short select_mode,
                                     bool draw_facedot,
                                     uint initial_offset,
                                     uint *r_vert_offset,
                                     uint *r_edge_offset,
                                     uint *r_face_offset)
{
  using namespace blender::draw;
  using namespace blender;
  Mesh &mesh = *static_cast<Mesh *>(ob->data);
  BMEditMesh *em = mesh.runtime->edit_mesh.get();

  BM_mesh_elem_table_ensure(em->bm, BM_VERT | BM_EDGE | BM_FACE);

  if (select_mode & SCE_SELECT_FACE) {
    gpu::Batch *geom_faces = DRW_mesh_batch_cache_get_triangles_with_select_id(mesh);
    PassSimple::Sub *face_sub = inst.select_face_flat;
    face_sub->push_constant("offset", int(initial_offset));
    face_sub->draw(geom_faces, res_handle);

    if (draw_facedot) {
      gpu::Batch *geom_facedots = DRW_mesh_batch_cache_get_facedots_with_select_id(mesh);
      face_sub->draw(geom_facedots, res_handle);
    }
    *r_face_offset = initial_offset + em->bm->totface;
  }
  else {
    if (ob->dt >= OB_SOLID) {
#ifdef USE_CAGE_OCCLUSION
      gpu::Batch *geom_faces = DRW_mesh_batch_cache_get_triangles_with_select_id(mesh);
#else
      gpu::Batch *geom_faces = DRW_mesh_batch_cache_get_surface(mesh);
#endif
      inst.select_face_uniform->draw(geom_faces, res_handle);
    }
    *r_face_offset = initial_offset;
  }

  /* Unlike faces, only draw edges if edge select mode. */
  if (select_mode & SCE_SELECT_EDGE) {
    gpu::Batch *geom_edges = DRW_mesh_batch_cache_get_edges_with_select_id(mesh);
    inst.select_edge->push_constant("offset", int(*r_face_offset));
    inst.select_edge->draw(geom_edges, res_handle);
    *r_edge_offset = *r_face_offset + em->bm->totedge;
  }
  else {
    /* Note that `r_vert_offset` is calculated from `r_edge_offset`.
     * Otherwise the first vertex is never selected, see: #53512. */
    *r_edge_offset = *r_face_offset;
  }

  /* Unlike faces, only verts if vert select mode. */
  if (select_mode & SCE_SELECT_VERTEX) {
    gpu::Batch *geom_verts = DRW_mesh_batch_cache_get_verts_with_select_id(mesh);
    inst.select_vert->push_constant("offset", int(*r_edge_offset));
    inst.select_vert->draw(geom_verts, res_handle);
    *r_vert_offset = *r_edge_offset + em->bm->totvert;
  }
  else {
    *r_vert_offset = *r_edge_offset;
  }
}

static void draw_select_id_mesh(SELECTID_Instance &inst,
                                Object *ob,
                                ResourceHandle res_handle,
                                short select_mode,
                                uint initial_offset,
                                uint *r_vert_offset,
                                uint *r_edge_offset,
                                uint *r_face_offset)
{
  using namespace blender::draw;
  using namespace blender;
  Mesh &mesh = *static_cast<Mesh *>(ob->data);

  gpu::Batch *geom_faces = DRW_mesh_batch_cache_get_triangles_with_select_id(mesh);
  if (select_mode & SCE_SELECT_FACE) {
    inst.select_face_flat->push_constant("offset", int(initial_offset));
    inst.select_face_flat->draw(geom_faces, res_handle);
    *r_face_offset = initial_offset + mesh.faces_num;
  }
  else {
    /* Only draw faces to mask out verts, we don't want their selection ID's. */
    inst.select_face_uniform->draw(geom_faces, res_handle);
    *r_face_offset = initial_offset;
  }

  if (select_mode & SCE_SELECT_EDGE) {
    gpu::Batch *geom_edges = DRW_mesh_batch_cache_get_edges_with_select_id(mesh);
    inst.select_edge->push_constant("offset", int(*r_face_offset));
    inst.select_edge->draw(geom_edges, res_handle);
    *r_edge_offset = *r_face_offset + mesh.edges_num;
  }
  else {
    *r_edge_offset = *r_face_offset;
  }

  if (select_mode & SCE_SELECT_VERTEX) {
    gpu::Batch *geom_verts = DRW_mesh_batch_cache_get_verts_with_select_id(mesh);
    inst.select_vert->push_constant("offset", int(*r_edge_offset));
    inst.select_vert->draw(geom_verts, res_handle);
    *r_vert_offset = *r_edge_offset + mesh.verts_num;
  }
  else {
    *r_vert_offset = *r_edge_offset;
  }
}

void select_id_draw_object(SELECTID_Instance &inst,
                           View3D *v3d,
                           Object *ob,
                           ResourceHandle res_handle,
                           short select_mode,
                           uint initial_offset,
                           uint *r_vert_offset,
                           uint *r_edge_offset,
                           uint *r_face_offset)
{
  BLI_assert(initial_offset > 0);

  switch (ob->type) {
    case OB_MESH: {
      const Mesh &mesh = *static_cast<const Mesh *>(ob->data);
      if (mesh.runtime->edit_mesh) {
        bool draw_facedot = check_ob_drawface_dot(select_mode, v3d, eDrawType(ob->dt));
        draw_select_id_edit_mesh(inst,
                                 ob,
                                 res_handle,
                                 select_mode,
                                 draw_facedot,
                                 initial_offset,
                                 r_vert_offset,
                                 r_edge_offset,
                                 r_face_offset);
      }
      else {
        draw_select_id_mesh(inst,
                            ob,
                            res_handle,
                            select_mode,
                            initial_offset,
                            r_vert_offset,
                            r_edge_offset,
                            r_face_offset);
      }
      break;
    }
    case OB_CURVES_LEGACY:
    case OB_SURF:
      break;
  }
}

/** \} */

#undef SELECT_ENGINE

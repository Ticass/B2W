#include <stdio.h>
#include <stdlib.h>
#include "ufbx.h"

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 3;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    void *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) return 4;
    fclose(f);
    ufbx_load_opts opts = {0};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.generate_missing_normals = true;
    ufbx_error error;
    ufbx_scene *scene = ufbx_load_memory(data, (size_t)n, &opts, &error);
    free(data);
    if (!scene) { fprintf(stderr, "parse error: %s\n", error.description.data); return 1; }
    printf("nodes=%zu meshes=%zu materials=%zu\n", scene->nodes.count, scene->meshes.count, scene->materials.count);
    for (size_t i = 0; i < scene->meshes.count; ++i) {
        ufbx_mesh *m = scene->meshes.data[i];
        printf("mesh[%zu] name=%s vertices=%zu indices=%zu faces=%zu triangles=%zu parts=%zu mats=%zu normal=%d uv=%d\n",
            i, m->name.data, m->num_vertices, m->num_indices, m->num_faces, m->num_triangles,
            m->material_parts.count, m->materials.count, m->vertex_normal.exists, m->vertex_uv.exists);
        printf(" instances=%zu max_face_triangles=%zu vertex_indices=%zu\n", m->instances.count, m->max_face_triangles, m->vertex_indices.count);
        for (size_t p = 0; p < m->material_parts.count; ++p) {
            ufbx_mesh_part *part = &m->material_parts.data[p];
            printf(" part[%zu] index=%u faces=%zu triangles=%zu face_indices=%zu\n", p, part->index, part->num_faces, part->num_triangles, part->face_indices.count);
        }
    }
    ufbx_free_scene(scene);
    return 0;
}

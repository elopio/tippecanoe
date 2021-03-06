#ifdef MTRACE
#include <mcheck.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <pthread.h>
#include <vector>
#include <algorithm>
#include <set>
#include <map>
#include <string>
#include "jsonpull/jsonpull.h"
#include "pool.hpp"
#include "projection.hpp"
#include "memfile.hpp"
#include "main.hpp"
#include "mbtiles.hpp"
#include "geojson.hpp"
#include "geometry.hpp"
#include "options.hpp"
#include "serial.hpp"
#include "text.hpp"
#include "read_json.hpp"
#include "mvt.hpp"

static long long parse_geometry1(int t, json_object *j, long long *bbox, drawvec &geom, int op, const char *fname, int line, int *initialized, unsigned *initial_x, unsigned *initial_y, json_object *feature, long long &prev, long long &offset, bool &has_prev) {
	parse_geometry(t, j, geom, op, fname, line, feature);

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO || geom[i].op == VT_LINETO) {
			long long x = geom[i].x;
			long long y = geom[i].y;

			if (additional[A_DETECT_WRAPAROUND]) {
				x += offset;
				if (has_prev) {
					if (x - prev > (1LL << 31)) {
						offset -= 1LL << 32;
						x -= 1LL << 32;
					} else if (prev - x > (1LL << 31)) {
						offset += 1LL << 32;
						x += 1LL << 32;
					}
				}

				has_prev = true;
				prev = x;
			}

			if (x < bbox[0]) {
				bbox[0] = x;
			}
			if (y < bbox[1]) {
				bbox[1] = y;
			}
			if (x > bbox[2]) {
				bbox[2] = x;
			}
			if (y > bbox[3]) {
				bbox[3] = y;
			}

			if (!*initialized) {
				if (x < 0 || x >= (1LL << 32) || y < 0 || y >= (1LL < 32)) {
					*initial_x = 1LL << 31;
					*initial_y = 1LL << 31;
				} else {
					*initial_x = (x >> geometry_scale) << geometry_scale;
					*initial_y = (y >> geometry_scale) << geometry_scale;
				}

				*initialized = 1;
			}

			geom[i].x = x >> geometry_scale;
			geom[i].y = y >> geometry_scale;
		}
	}

	return geom.size();
}

int serialize_geometry(json_object *geometry, json_object *properties, json_object *id, const char *reading, int line, volatile long long *layer_seq, volatile long long *progress_seq, long long *metapos, long long *geompos, long long *indexpos, std::set<std::string> *exclude, std::set<std::string> *include, int exclude_all, FILE *metafile, FILE *geomfile, FILE *indexfile, struct memfile *poolfile, struct memfile *treefile, const char *fname, int basezoom, int layer, double droprate, long long *file_bbox, json_object *tippecanoe, int segment, int *initialized, unsigned *initial_x, unsigned *initial_y, struct reader *readers, int maxzoom, json_object *feature, std::map<std::string, layermap_entry> *layermap, std::string layername, bool uses_gamma, std::map<std::string, int> const *attribute_types, double *dist_sum, size_t *dist_count, bool want_dist, bool filters) {
	json_object *geometry_type = json_hash_get(geometry, "type");
	if (geometry_type == NULL) {
		static int warned = 0;
		if (!warned) {
			fprintf(stderr, "%s:%d: null geometry (additional not reported)\n", reading, line);
			json_context(feature);
			warned = 1;
		}

		return 0;
	}

	if (geometry_type->type != JSON_STRING) {
		fprintf(stderr, "%s:%d: geometry type is not a string\n", reading, line);
		json_context(feature);
		return 0;
	}

	json_object *coordinates = json_hash_get(geometry, "coordinates");
	if (coordinates == NULL || coordinates->type != JSON_ARRAY) {
		fprintf(stderr, "%s:%d: feature without coordinates array\n", reading, line);
		json_context(feature);
		return 0;
	}

	int t;
	for (t = 0; t < GEOM_TYPES; t++) {
		if (strcmp(geometry_type->string, geometry_names[t]) == 0) {
			break;
		}
	}
	if (t >= GEOM_TYPES) {
		fprintf(stderr, "%s:%d: Can't handle geometry type %s\n", reading, line, geometry_type->string);
		json_context(feature);
		return 0;
	}

	int tippecanoe_minzoom = -1;
	int tippecanoe_maxzoom = -1;
	std::string tippecanoe_layername;

	if (tippecanoe != NULL) {
		json_object *min = json_hash_get(tippecanoe, "minzoom");
		if (min != NULL && min->type == JSON_NUMBER) {
			tippecanoe_minzoom = min->number;
		}
		if (min != NULL && min->type == JSON_STRING) {
			tippecanoe_minzoom = atoi(min->string);
		}

		json_object *max = json_hash_get(tippecanoe, "maxzoom");
		if (max != NULL && max->type == JSON_NUMBER) {
			tippecanoe_maxzoom = max->number;
		}
		if (max != NULL && max->type == JSON_STRING) {
			tippecanoe_maxzoom = atoi(max->string);
		}

		json_object *ln = json_hash_get(tippecanoe, "layer");
		if (ln != NULL && (ln->type == JSON_STRING || ln->type == JSON_NUMBER)) {
			tippecanoe_layername = std::string(ln->string);
		}
	}

	bool has_id = false;
	unsigned long long id_value = 0;
	if (id != NULL) {
		if (id->type == JSON_NUMBER) {
			if (id->number >= 0) {
				char *err = NULL;
				id_value = strtoull(id->string, &err, 10);

				if (err != NULL && *err != '\0') {
					static bool warned_frac = false;

					if (!warned_frac) {
						fprintf(stderr, "Warning: Can't represent non-integer feature ID %s\n", id->string);
						warned_frac = true;
					}
				} else {
					has_id = true;
				}
			} else {
				static bool warned_neg = false;

				if (!warned_neg) {
					fprintf(stderr, "Warning: Can't represent negative feature ID %s\n", id->string);
					warned_neg = true;
				}
			}
		} else {
			static bool warned_nan = false;

			if (!warned_nan) {
				char *s = json_stringify(id);
				fprintf(stderr, "Warning: Can't represent non-numeric feature ID %s\n", s);
				free(s);  // stringify
				warned_nan = true;
			}
		}
	}

	long long bbox[] = {LLONG_MAX, LLONG_MAX, LLONG_MIN, LLONG_MIN};

	if (!filters) {
		if (tippecanoe_layername.size() != 0) {
			if (layermap->count(tippecanoe_layername) == 0) {
				layermap->insert(std::pair<std::string, layermap_entry>(tippecanoe_layername, layermap_entry(layermap->size())));
			}

			auto ai = layermap->find(tippecanoe_layername);
			if (ai != layermap->end()) {
				layer = ai->second.id;
				layername = tippecanoe_layername;

				if (mb_geometry[t] == VT_POINT) {
					ai->second.points++;
				} else if (mb_geometry[t] == VT_LINE) {
					ai->second.lines++;
				} else if (mb_geometry[t] == VT_POLYGON) {
					ai->second.polygons++;
				}
			} else {
				fprintf(stderr, "Internal error: can't find layer name %s\n", tippecanoe_layername.c_str());
				exit(EXIT_FAILURE);
			}
		} else {
			auto fk = layermap->find(layername);
			if (fk != layermap->end()) {
				if (mb_geometry[t] == VT_POINT) {
					fk->second.points++;
				} else if (mb_geometry[t] == VT_LINE) {
					fk->second.lines++;
				} else if (mb_geometry[t] == VT_POLYGON) {
					fk->second.polygons++;
				}
			}
		}
	}

	size_t nprop = 0;
	if (properties != NULL && properties->type == JSON_HASH) {
		nprop = properties->length;
	}

	char *metakey[nprop];
	std::vector<std::string> metaval;
	metaval.resize(nprop);
	int metatype[nprop];
	size_t m = 0;

	for (size_t i = 0; i < nprop; i++) {
		if (properties->keys[i]->type == JSON_STRING) {
			std::string s(properties->keys[i]->string);

			if (exclude_all) {
				if (include->count(s) == 0) {
					continue;
				}
			} else if (exclude->count(s) != 0) {
				continue;
			}

			int type = -1;
			std::string val;
			stringify_value(properties->values[i], type, val, reading, line, feature, properties->keys[i]->string, attribute_types);

			if (type >= 0) {
				metakey[m] = properties->keys[i]->string;
				metatype[m] = type;
				metaval[m] = val;
				m++;

				type_and_string attrib;
				attrib.type = metatype[m - 1];
				attrib.string = metaval[m - 1];

				if (!filters) {
					auto fk = layermap->find(layername);
					add_to_file_keys(fk->second.file_keys, metakey[m - 1], attrib);
				}
			}
		}
	}

	bool has_prev = false;
	long long prev = 0;
	long long offset = 0;

	drawvec dv;
	long long g = parse_geometry1(t, coordinates, bbox, dv, VT_MOVETO, fname, line, initialized, initial_x, initial_y, feature, prev, offset, has_prev);
	if (mb_geometry[t] == VT_POLYGON) {
		dv = fix_polygon(dv);
	}

	if (want_dist) {
		std::vector<unsigned long long> locs;
		for (size_t i = 0; i < dv.size(); i++) {
			if (dv[i].op == VT_MOVETO || dv[i].op == VT_LINETO) {
				locs.push_back(encode(dv[i].x << geometry_scale, dv[i].y << geometry_scale));
			}
		}
		std::sort(locs.begin(), locs.end());
		size_t n = 0;
		double sum = 0;
		for (size_t i = 1; i < locs.size(); i++) {
			if (locs[i - 1] != locs[i]) {
				sum += log(locs[i] - locs[i - 1]);
				n++;
			}
		}
		if (n > 0) {
			double avg = exp(sum / n);
			// Convert approximately from tile units to feet
			double dist_ft = sqrt(avg) / 33;

			*dist_sum += log(dist_ft) * n;
			*dist_count += n;
		}
		locs.clear();
	}

	bool inline_meta = true;
	// Don't inline metadata for features that will span several tiles at maxzoom
	if (g > 0 && (bbox[2] < bbox[0] || bbox[3] < bbox[1])) {
		fprintf(stderr, "Internal error: impossible feature bounding box %llx,%llx,%llx,%llx\n", bbox[0], bbox[1], bbox[2], bbox[3]);
	}
	if (bbox[2] - bbox[0] > (2LL << (32 - maxzoom)) || bbox[3] - bbox[1] > (2LL << (32 - maxzoom))) {
		inline_meta = false;

		if (prevent[P_CLIPPING]) {
			static volatile long long warned = 0;
			long long extent = ((bbox[2] - bbox[0]) / ((1LL << (32 - maxzoom)) + 1)) * ((bbox[3] - bbox[1]) / ((1LL << (32 - maxzoom)) + 1));
			if (extent > warned) {
				fprintf(stderr, "Warning: %s:%d: Large unclipped (-pc) feature may be duplicated across %lld tiles\n", fname, line, extent);
				warned = extent;

				if (extent > 10000) {
					fprintf(stderr, "Exiting because this can't be right.\n");
					exit(EXIT_FAILURE);
				}
			}
		}
	}

	double extent = 0;
	if (additional[A_DROP_SMALLEST_AS_NEEDED]) {
		if (mb_geometry[t] == VT_POLYGON) {
			for (size_t i = 0; i < dv.size(); i++) {
				if (dv[i].op == VT_MOVETO) {
					size_t j;
					for (j = i + 1; j < dv.size(); j++) {
						if (dv[j].op != VT_LINETO) {
							break;
						}
					}

					extent += get_area(dv, i, j);
					i = j - 1;
				}
			}
		} else if (mb_geometry[t] == VT_LINE) {
			for (size_t i = 1; i < dv.size(); i++) {
				if (dv[i].op == VT_LINETO) {
					double xd = dv[i].x - dv[i - 1].x;
					double yd = dv[i].y - dv[i - 1].y;
					extent += sqrt(xd * xd + yd * yd);
				}
			}
		}
	}

	long long geomstart = *geompos;
	long long bbox_index;

	serial_feature sf;
	sf.layer = layer;
	sf.segment = segment;
	sf.t = mb_geometry[t];
	sf.has_id = has_id;
	sf.id = id_value;
	sf.has_tippecanoe_minzoom = (tippecanoe_minzoom != -1);
	sf.tippecanoe_minzoom = tippecanoe_minzoom;
	sf.has_tippecanoe_maxzoom = (tippecanoe_maxzoom != -1);
	sf.tippecanoe_maxzoom = tippecanoe_maxzoom;
	sf.geometry = dv;
	sf.m = m;
	sf.feature_minzoom = 0;  // Will be filled in during index merging
	sf.extent = (long long) extent;

	if (prevent[P_INPUT_ORDER]) {
		sf.seq = *layer_seq;
	} else {
		sf.seq = 0;
	}

	// Calculate the center even if off the edge of the plane,
	// and then mask to bring it back into the addressable area
	long long midx = (bbox[0] / 2 + bbox[2] / 2) & ((1LL << 32) - 1);
	long long midy = (bbox[1] / 2 + bbox[3] / 2) & ((1LL << 32) - 1);
	bbox_index = encode(midx, midy);

	if (additional[A_DROP_DENSEST_AS_NEEDED] || additional[A_CALCULATE_FEATURE_DENSITY] || additional[A_INCREASE_GAMMA_AS_NEEDED] || uses_gamma) {
		sf.index = bbox_index;
	} else {
		sf.index = 0;
	}

	if (inline_meta) {
		sf.metapos = -1;
		for (size_t i = 0; i < m; i++) {
			sf.keys.push_back(addpool(poolfile, treefile, metakey[i], mvt_string));
			sf.values.push_back(addpool(poolfile, treefile, metaval[i].c_str(), metatype[i]));
		}
	} else {
		sf.metapos = *metapos;
		for (size_t i = 0; i < m; i++) {
			serialize_long_long(metafile, addpool(poolfile, treefile, metakey[i], mvt_string), metapos, fname);
			serialize_long_long(metafile, addpool(poolfile, treefile, metaval[i].c_str(), metatype[i]), metapos, fname);
		}
	}

	serialize_feature(geomfile, &sf, geompos, fname, *initial_x >> geometry_scale, *initial_y >> geometry_scale, false);

	struct index index;
	index.start = geomstart;
	index.end = *geompos;
	index.segment = segment;
	index.seq = *layer_seq;
	index.t = sf.t;
	index.index = bbox_index;

	fwrite_check(&index, sizeof(struct index), 1, indexfile, fname);
	*indexpos += sizeof(struct index);

	for (size_t i = 0; i < 2; i++) {
		if (bbox[i] < file_bbox[i]) {
			file_bbox[i] = bbox[i];
		}
	}
	for (size_t i = 2; i < 4; i++) {
		if (bbox[i] > file_bbox[i]) {
			file_bbox[i] = bbox[i];
		}
	}

	if (*progress_seq % 10000 == 0) {
		checkdisk(readers, CPUS);
		if (!quiet) {
			fprintf(stderr, "Read %.2f million features\r", *progress_seq / 1000000.0);
		}
	}
	(*progress_seq)++;
	(*layer_seq)++;

	return 1;
}

void check_crs(json_object *j, const char *reading) {
	json_object *crs = json_hash_get(j, "crs");
	if (crs != NULL) {
		json_object *properties = json_hash_get(crs, "properties");
		if (properties != NULL) {
			json_object *name = json_hash_get(properties, "name");
			if (name->type == JSON_STRING) {
				if (strcmp(name->string, projection->alias) != 0) {
					fprintf(stderr, "%s: Warning: GeoJSON specified projection \"%s\", not the expected \"%s\".\n", reading, name->string, projection->alias);
					fprintf(stderr, "%s: If \"%s\" is not the expected projection, use -s to specify the right one.\n", reading, projection->alias);
				}
			}
		}
	}
}

void parse_json(json_pull *jp, const char *reading, volatile long long *layer_seq, volatile long long *progress_seq, long long *metapos, long long *geompos, long long *indexpos, std::set<std::string> *exclude, std::set<std::string> *include, int exclude_all, FILE *metafile, FILE *geomfile, FILE *indexfile, struct memfile *poolfile, struct memfile *treefile, char *fname, int basezoom, int layer, double droprate, long long *file_bbox, int segment, int *initialized, unsigned *initial_x, unsigned *initial_y, struct reader *readers, int maxzoom, std::map<std::string, layermap_entry> *layermap, std::string layername, bool uses_gamma, std::map<std::string, int> const *attribute_types, double *dist_sum, size_t *dist_count, bool want_dist, bool filters) {
	long long found_hashes = 0;
	long long found_features = 0;
	long long found_geometries = 0;

	while (1) {
		json_object *j = json_read(jp);
		if (j == NULL) {
			if (jp->error != NULL) {
				fprintf(stderr, "%s:%d: %s\n", reading, jp->line, jp->error);
				if (jp->root != NULL) {
					json_context(jp->root);
				}
			}

			json_free(jp->root);
			break;
		}

		if (j->type == JSON_HASH) {
			found_hashes++;

			if (found_hashes == 50 && found_features == 0 && found_geometries == 0) {
				fprintf(stderr, "%s:%d: Warning: not finding any GeoJSON features or geometries in input yet after 50 objects.\n", reading, jp->line);
			}
		}

		json_object *type = json_hash_get(j, "type");
		if (type == NULL || type->type != JSON_STRING) {
			continue;
		}

		if (found_features == 0) {
			int i;
			int is_geometry = 0;
			for (i = 0; i < GEOM_TYPES; i++) {
				if (strcmp(type->string, geometry_names[i]) == 0) {
					is_geometry = 1;
					break;
				}
			}

			if (is_geometry) {
				if (j->parent != NULL) {
					if (j->parent->type == JSON_ARRAY) {
						if (j->parent->parent->type == JSON_HASH) {
							json_object *geometries = json_hash_get(j->parent->parent, "geometries");
							if (geometries != NULL) {
								// Parent of Parent must be a GeometryCollection
								is_geometry = 0;
							}
						}
					} else if (j->parent->type == JSON_HASH) {
						json_object *geometry = json_hash_get(j->parent, "geometry");
						if (geometry != NULL) {
							// Parent must be a Feature
							is_geometry = 0;
						}
					}
				}
			}

			if (is_geometry) {
				if (found_features != 0 && found_geometries == 0) {
					fprintf(stderr, "%s:%d: Warning: found a mixture of features and bare geometries\n", reading, jp->line);
				}
				found_geometries++;

				serialize_geometry(j, NULL, NULL, reading, jp->line, layer_seq, progress_seq, metapos, geompos, indexpos, exclude, include, exclude_all, metafile, geomfile, indexfile, poolfile, treefile, fname, basezoom, layer, droprate, file_bbox, NULL, segment, initialized, initial_x, initial_y, readers, maxzoom, j, layermap, layername, uses_gamma, attribute_types, dist_sum, dist_count, want_dist, filters);
				json_free(j);
				continue;
			}
		}

		if (strcmp(type->string, "Feature") != 0) {
			if (strcmp(type->string, "FeatureCollection") == 0) {
				check_crs(j, reading);
				json_free(j);
			}

			continue;
		}

		if (found_features == 0 && found_geometries != 0) {
			fprintf(stderr, "%s:%d: Warning: found a mixture of features and bare geometries\n", reading, jp->line);
		}
		found_features++;

		json_object *geometry = json_hash_get(j, "geometry");
		if (geometry == NULL) {
			fprintf(stderr, "%s:%d: feature with no geometry\n", reading, jp->line);
			json_context(j);
			json_free(j);
			continue;
		}

		json_object *properties = json_hash_get(j, "properties");
		if (properties == NULL || (properties->type != JSON_HASH && properties->type != JSON_NULL)) {
			fprintf(stderr, "%s:%d: feature without properties hash\n", reading, jp->line);
			json_context(j);
			json_free(j);
			continue;
		}

		json_object *tippecanoe = json_hash_get(j, "tippecanoe");
		json_object *id = json_hash_get(j, "id");

		json_object *geometries = json_hash_get(geometry, "geometries");
		if (geometries != NULL) {
			size_t g;
			for (g = 0; g < geometries->length; g++) {
				serialize_geometry(geometries->array[g], properties, id, reading, jp->line, layer_seq, progress_seq, metapos, geompos, indexpos, exclude, include, exclude_all, metafile, geomfile, indexfile, poolfile, treefile, fname, basezoom, layer, droprate, file_bbox, tippecanoe, segment, initialized, initial_x, initial_y, readers, maxzoom, j, layermap, layername, uses_gamma, attribute_types, dist_sum, dist_count, want_dist, filters);
			}
		} else {
			serialize_geometry(geometry, properties, id, reading, jp->line, layer_seq, progress_seq, metapos, geompos, indexpos, exclude, include, exclude_all, metafile, geomfile, indexfile, poolfile, treefile, fname, basezoom, layer, droprate, file_bbox, tippecanoe, segment, initialized, initial_x, initial_y, readers, maxzoom, j, layermap, layername, uses_gamma, attribute_types, dist_sum, dist_count, want_dist, filters);
		}

		json_free(j);

		/* XXX check for any non-features in the outer object */
	}
}

void *run_parse_json(void *v) {
	struct parse_json_args *pja = (struct parse_json_args *) v;

	parse_json(pja->jp, pja->reading, pja->layer_seq, pja->progress_seq, pja->metapos, pja->geompos, pja->indexpos, pja->exclude, pja->include, pja->exclude_all, pja->metafile, pja->geomfile, pja->indexfile, pja->poolfile, pja->treefile, pja->fname, pja->basezoom, pja->layer, pja->droprate, pja->file_bbox, pja->segment, pja->initialized, pja->initial_x, pja->initial_y, pja->readers, pja->maxzoom, pja->layermap, *pja->layername, pja->uses_gamma, pja->attribute_types, pja->dist_sum, pja->dist_count, pja->want_dist, pja->filters);

	return NULL;
}

struct jsonmap {
	char *map;
	unsigned long long off;
	unsigned long long end;
};

ssize_t json_map_read(struct json_pull *jp, char *buffer, size_t n) {
	struct jsonmap *jm = (struct jsonmap *) jp->source;

	if (jm->off + n >= jm->end) {
		n = jm->end - jm->off;
	}

	memcpy(buffer, jm->map + jm->off, n);
	jm->off += n;

	return n;
}

struct json_pull *json_begin_map(char *map, long long len) {
	struct jsonmap *jm = new jsonmap;
	if (jm == NULL) {
		perror("Out of memory");
		exit(EXIT_FAILURE);
	}

	jm->map = map;
	jm->off = 0;
	jm->end = len;

	return json_begin(json_map_read, jm);
}

void json_end_map(struct json_pull *jp) {
	delete (struct jsonmap *) jp->source;
	json_end(jp);
}

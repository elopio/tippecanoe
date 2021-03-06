#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <map>

extern "C" {
#include "jsonpull/jsonpull.h"
}

#include "geometry.hpp"
#include "projection.hpp"
#include "read_json.hpp"
#include "text.hpp"
#include "mvt.hpp"

const char *geometry_names[GEOM_TYPES] = {
	"Point", "MultiPoint", "LineString", "MultiLineString", "Polygon", "MultiPolygon",
};

int geometry_within[GEOM_TYPES] = {
	-1,		 /* point */
	GEOM_POINT,      /* multipoint */
	GEOM_POINT,      /* linestring */
	GEOM_LINESTRING, /* multilinestring */
	GEOM_LINESTRING, /* polygon */
	GEOM_POLYGON,    /* multipolygon */
};

int mb_geometry[GEOM_TYPES] = {
	VT_POINT, VT_POINT, VT_LINE, VT_LINE, VT_POLYGON, VT_POLYGON,
};

void json_context(json_object *j) {
	char *s = json_stringify(j);

	if (strlen(s) >= 500) {
		sprintf(s + 497, "...");
	}

	fprintf(stderr, "In JSON object %s\n", s);
	free(s);  // stringify
}

void parse_geometry(int t, json_object *j, drawvec &out, int op, const char *fname, int line, json_object *feature) {
	if (j == NULL || j->type != JSON_ARRAY) {
		fprintf(stderr, "%s:%d: expected array for type %d\n", fname, line, t);
		json_context(feature);
		return;
	}

	int within = geometry_within[t];
	if (within >= 0) {
		size_t i;
		for (i = 0; i < j->length; i++) {
			if (within == GEOM_POINT) {
				if (i == 0 || mb_geometry[t] == GEOM_MULTIPOINT) {
					op = VT_MOVETO;
				} else {
					op = VT_LINETO;
				}
			}

			parse_geometry(within, j->array[i], out, op, fname, line, feature);
		}
	} else {
		if (j->length >= 2 && j->array[0]->type == JSON_NUMBER && j->array[1]->type == JSON_NUMBER) {
			long long x, y;
			double lon = j->array[0]->number;
			double lat = j->array[1]->number;
			projection->project(lon, lat, 32, &x, &y);

			if (j->length > 2) {
				static int warned = 0;

				if (!warned) {
					fprintf(stderr, "%s:%d: ignoring dimensions beyond two\n", fname, line);
					json_context(j);
					json_context(feature);
					warned = 1;
				}
			}

			draw d(op, x, y);
			out.push_back(draw(op, x, y));
		} else {
			fprintf(stderr, "%s:%d: malformed point\n", fname, line);
			json_context(j);
			json_context(feature);
		}
	}

	if (t == GEOM_POLYGON) {
		// Note that this is not using the correct meaning of closepath.
		//
		// We are using it here to close an entire Polygon, to distinguish
		// the Polygons within a MultiPolygon from each other.
		//
		// This will be undone in fix_polygon(), which needs to know which
		// rings come from which Polygons so that it can make the winding order
		// of the outer ring be the opposite of the order of the inner rings.

		out.push_back(draw(VT_CLOSEPATH, 0, 0));
	}
}

void stringify_value(json_object *value, int &type, std::string &stringified, const char *reading, int line, json_object *feature, std::string const &key, std::map<std::string, int> const *attribute_types) {
	if (value != NULL) {
		int vt = value->type;
		std::string val;

		if (vt == JSON_STRING || vt == JSON_NUMBER) {
			val = value->string;
		} else if (vt == JSON_TRUE) {
			val = "true";
		} else if (vt == JSON_FALSE) {
			val = "false";
		} else if (vt == JSON_NULL) {
			val = "null";
		} else {
			const char *v = json_stringify(value);
			val = std::string(v);
			free((void *) v);  // stringify
		}

		auto a = (*attribute_types).find(key);
		if (a != attribute_types->end()) {
			if (a->second == mvt_string) {
				vt = JSON_STRING;
			} else if (a->second == mvt_float) {
				vt = JSON_NUMBER;
				val = std::to_string(atof(val.c_str()));
			} else if (a->second == mvt_int) {
				vt = JSON_NUMBER;
				if (val.size() == 0) {
					val = "0";
				}

				for (size_t ii = 0; ii < val.size(); ii++) {
					char c = val[ii];
					if (c < '0' || c > '9') {
						val = std::to_string(round(atof(val.c_str())));
						break;
					}
				}
			} else if (a->second == mvt_bool) {
				if (val == "false" || val == "0" || val == "null" || val.size() == 0) {
					vt = JSON_FALSE;
					val = "false";
				} else {
					vt = JSON_TRUE;
					val = "true";
				}
			} else {
				fprintf(stderr, "Can't happen: attribute type %d\n", a->second);
				exit(EXIT_FAILURE);
			}
		}

		if (vt == JSON_STRING) {
			type = mvt_string;
			stringified = val;
			std::string err = check_utf8(val);
			if (err != "") {
				fprintf(stderr, "%s:%d: %s\n", reading, line, err.c_str());
				json_context(feature);
				exit(EXIT_FAILURE);
			}
		} else if (vt == JSON_NUMBER) {
			type = mvt_double;
			stringified = val;
		} else if (vt == JSON_TRUE || vt == JSON_FALSE) {
			type = mvt_bool;
			stringified = val;
		} else if (vt == JSON_NULL) {
			;
		} else {
			type = mvt_string;
			stringified = val;
		}
	}
}

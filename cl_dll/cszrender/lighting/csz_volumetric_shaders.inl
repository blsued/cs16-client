// csz_volumetric_shaders.inl -- inline GLSL for the volumetric light cone.
//
// SPDX-License-Identifier: GPL-2.0-or-later
// Part of CSOZ (cs16-client fork). Clean-room; no code copied or translated
// from PrimeXT/Paranoia/Trinity/CSMoE/retail. #version 330 core, written to
// stay GLES3-compatible (no double, no compute, textureProj only).
//
// Mechanism (V5, depth-correct): a world-space cone hull (apex=light origin,
// axis=light dir) is drawn additively with depth-test ON (GL_LEQUAL) / depth-
// write OFF, drawing BACK faces only (front faces culled). Hardware depth-test
// therefore rejects any hull fragment behind an opaque surface, so a wall
// BETWEEN the camera and the cone fully occludes the shaft for free; drawing
// back faces keeps the footprint valid when the camera is inside the cone.
// For each surviving fragment the FS computes the closed-form ray-vs-infinite-
// cone intersection (clipped to the [0,u_radius] axial span and the base cap),
// then integrates in-scatter ONLY over the chord actually inside the cone
// [tEntry,tExit] -- bounded by u_steps, never an unbounded march.
// At MED/HIGH the FS also clamps tExit to the scene depth read from a once-
// allocated depth-copy texture (u_hasSceneDepth=1), which occludes interior
// walls / spot-on-wall cases that hardware depth-test alone cannot clamp.

static const char kVolumetricVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;       // cone vertex, world space
uniform mat4 u_viewProj;
out vec3 v_worldPos;
void main()
{
	v_worldPos = a_pos;
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

static const char kVolumetricFs[] = R"GLSL(#version 330 core
in vec3 v_worldPos;                       // back-face hull point of the cone (this fragment)
uniform vec3 u_camPos;                    // ray origin (camera)
uniform vec3 u_lightOrigin;               // cone apex
uniform vec3 u_lightDir;                  // normalized cone axis
uniform vec3 u_lightColor;                // linear, intensity-premultiplied; the cap ceiling
uniform float u_radius;                   // cone length / attenuation end
uniform float u_cosInner;                 // full-intensity inner cone (cos half-angle)
uniform float u_cosOuter;                 // cone cutoff (cos half-angle)
uniform float u_scatter;                  // scatter coefficient (per-unit in-scatter rate)
uniform float u_strength;                 // soft-cap exposure scale (dev tunable)
uniform float u_fogDensity;               // view.ambience.fogDensity; more fog -> denser shaft
uniform int u_steps;                      // bounded integration sample count
// Shadow carve (high tier): same projection as the world lit pass.
uniform mat4 u_matShadow;
uniform sampler2DShadow u_shadowMap;      // unit 2 (sampled only when u_hasShadow != 0)
uniform int u_hasShadow;
// Scene-depth clamp (MED/HIGH only): a plain (non-compare) depth copy of the
// opaque scene; sampled only when u_hasSceneDepth != 0.
uniform sampler2D u_sceneDepth;           // unit 3, GL_DEPTH_COMPONENT24, NEAREST
uniform int u_hasSceneDepth;
uniform vec2 u_screenSize;                // viewport w,h for the depth texel fetch
uniform float u_zNear;                    // view.zNear
uniform float u_zFar;                     // view.zFar
out vec4 fragColor;

// DEPTH BEHAVIOR (V5): occlusion is real, in two layers.
//  1) Hardware: the cone is drawn depth-tested (LEQUAL) as a back-face hull, so
//     a wall in front of the cone z-rejects every hull fragment -> the shaft is
//     fully hidden behind such a wall at zero shader cost (ALL tiers).
//  2) Scene-depth clamp (MED/HIGH): the integration exit is clamped to the
//     opaque scene depth (min(volumeExit, sceneDepth)), so a wall standing
//     INSIDE the cone (or the surface the spot lands on) cuts the shaft at the
//     wall instead of bleeding past it. OFF/LOW skip this sample entirely
//     (u_hasSceneDepth==0) so they keep the cheap path.
// The high tier additionally carves shadowed sub-segments out of the integral.

// Linearize a non-linear [0,1] depth-buffer value to eye-space distance.
float LinearizeDepth( float d )
{
	float z = d * 2.0 - 1.0;	// NDC z
	return ( 2.0 * u_zNear * u_zFar ) / ( u_zFar + u_zNear - z * ( u_zFar - u_zNear ) );
}

void main()
{
	vec3 ro = u_camPos;
	vec3 rd = v_worldPos - ro;
	float hullLen = length( rd );
	if( hullLen < 1e-4 )
		discard;
	rd /= hullLen;

	// --- Closed-form ray vs INFINITE cone (apex u_lightOrigin, axis u_lightDir,
	// half-angle from u_cosOuter). Solve |proj_axis|^2 = cos^2 * |v|^2 where
	// v = (ro + t*rd) - apex. Quadratic A t^2 + B t + C = 0. ---
	float cosA = clamp( u_cosOuter, 1e-3, 0.9999 );
	float k = cosA * cosA;
	vec3 co = ro - u_lightOrigin;
	float dDir = dot( rd, u_lightDir );
	float coDir = dot( co, u_lightDir );

	float A = dDir * dDir - k;
	float B = 2.0 * ( dDir * coDir - dot( rd, co ) * k );
	float C = coDir * coDir - dot( co, co ) * k;

	// Default: empty interval (no contribution).
	float tEntry = 1.0;
	float tExit = 0.0;

	const float kEps = 1e-5;
	if( abs( A ) < kEps )
	{
		// Degenerate (ray nearly parallel to the cone surface): linear B t + C = 0.
		if( abs( B ) > kEps )
		{
			float t = -C / B;
			// Half-space test selects the correct nappe; pick a tiny span so the
			// midpoint test below still profiles it. Treat as grazing: skip.
			tEntry = t;
			tExit = t;
		}
	}
	else
	{
		float disc = B * B - 4.0 * A * C;
		if( disc >= 0.0 )
		{
			float sq = sqrt( disc );
			float t0 = ( -B - sq ) / ( 2.0 * A );
			float t1 = ( -B + sq ) / ( 2.0 * A );
			if( t0 > t1 ) { float tmp = t0; t0 = t1; t1 = tmp; }
			tEntry = t0;
			tExit = t1;
		}
	}

	// The quadratic also accepts the mirror (back) nappe. Clip both ends to the
	// valid axial span [0, u_radius] measured along the cone axis: a point at
	// ray param t has axial distance s(t) = coDir + t*dDir. We need 0 <= s <= R.
	// Re-derive entry/exit on the ray as the overlap of [tEntry,tExit] with the
	// axial slab. Solve s(t)=0 and s(t)=u_radius for the slab bounds on t.
	if( tExit > tEntry )
	{
		if( abs( dDir ) > kEps )
		{
			float tA = ( 0.0 - coDir ) / dDir;          // axial dist 0 (apex plane)
			float tB = ( u_radius - coDir ) / dDir;     // axial dist u_radius (base cap)
			float slabLo = min( tA, tB );
			float slabHi = max( tA, tB );
			tEntry = max( tEntry, slabLo );
			tExit = min( tExit, slabHi );
		}
		else
		{
			// Ray perpendicular to axis: axial dist is constant = coDir.
			if( coDir < 0.0 || coDir > u_radius )
			{
				tEntry = 1.0; tExit = 0.0;	// outside the slab everywhere
			}
		}
	}

	// Camera inside the cone integrates from the camera; never past the hull.
	tEntry = max( tEntry, 0.0 );
	tExit = min( tExit, hullLen );

	// Scene-depth clamp (MED/HIGH): cut the exit at the first opaque surface
	// along the ray. The depth copy is in the SAME projection as the hull, so we
	// compare in eye-distance and convert to a ray param via the hull metric
	// (hullLen at this fragment maps to its eye distance through the same rd).
	if( u_hasSceneDepth != 0 )
	{
		vec2 uv = gl_FragCoord.xy / u_screenSize;
		float sceneEye = LinearizeDepth( texture( u_sceneDepth, uv ).r );
		// Convert eye distance (along view forward) to distance along rd: the
		// fragment's own eye distance is sceneEye-independent, so use the ratio
		// of ray length to eye-z. We do not have the view matrix here, so use the
		// hull as the calibration: hullLen corresponds to this fragment's eye
		// distance. cosView = (hull eye-z)/hullLen, but eye-z of the hull equals
		// its own LinearizeDepth(gl_FragCoord.z). tScene = sceneEye * hullLen /
		// hullEye keeps both in the same metric.
		float hullEye = LinearizeDepth( gl_FragCoord.z );
		float tScene = ( hullEye > 1e-4 ) ? ( sceneEye * hullLen / hullEye ) : tExit;
		tExit = min( tExit, tScene );
	}

	if( tExit <= tEntry )
	{
		fragColor = vec4( 0.0, 0.0, 0.0, 1.0 );	// additive: contributes nothing
		return;
	}

	// --- Bounded integration over the in-cone chord [tEntry, tExit] only. ---
	int steps = u_steps;
	float span = tExit - tEntry;
	float stepLen = span / float( steps );
	float inScatter = 0.0;

	// Normalize the fog-density influence to the Round-1 default (0.0018) so the
	// default scatter coefficient is tuned for the shipped fog; denser fog gives
	// a stronger shaft, lighter fog a fainter one. 0.5 floor so thin fog still
	// shows a shaft.
	float fogFactor = clamp( u_fogDensity / 0.0018, 0.5, 4.0 );

	// Half-step offset so samples sit at segment centers (midpoint integration).
	for( int i = 0; i < steps; ++i )
	{
		vec3 p = ro + rd * ( tEntry + ( float( i ) + 0.5 ) * stepLen );

		vec3 toP = p - u_lightOrigin;
		float dp = length( toP );
		if( dp > u_radius || dp < 1e-4 )
			continue;
		vec3 dirP = toP / dp;

		float c = dot( dirP, u_lightDir );
		if( c <= u_cosOuter )
			continue;	// outside the cone (guards the analytic edges)

		// Radial soft edge (smoothstep over the cone's soft band) x distance
		// falloff; 0.25 isotropic floor so the shaft is visible side-on,
		// 0.75*dist keeps the near-apex region brighter.
		float edge = smoothstep( u_cosOuter, u_cosInner, c );
		float dist = 1.0 - dp / u_radius;
		float profile = edge * ( 0.25 + 0.75 * dist );

		float shadowFactor = 1.0;
		if( u_hasShadow != 0 )
			shadowFactor = textureProj( u_shadowMap, u_matShadow * vec4( p, 1.0 ));

		inScatter += profile * shadowFactor * stepLen * u_scatter * fogFactor;
	}

	// SOFT CAP (the "visible but not over-exposed" requirement): in 8-bit
	// additive, exp saturation keeps the shaft below lightColor forever, so it
	// reads as soft light-in-air, never a solid white beam.
	vec3 col = u_lightColor * ( 1.0 - exp( -inScatter * u_strength ) );
	col = min( col, u_lightColor * 0.85 );	// leave 15% headroom; never solid white
	fragColor = vec4( col, 1.0 );
}
)GLSL";

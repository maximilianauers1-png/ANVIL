#version 450
// Procedural environment behind the scene (Studio theme): sunset sea, fjord,
// fjord at night. Depends only on the view direction (an environment at
// infinity, like an HDRI), so it turns with the camera and costs one
// full-screen pass. All assets are procedural (no textures, no downloads).
//
// Sky radiance: single scattering in a spherical atmosphere with exponential
// Rayleigh/Mie density and an ozone layer, integrated numerically along the
// view ray (Nishita et al. 1993, "Display of the Earth taking into account
// atmospheric scattering"; parameters as in Bruneton 2017, "A qualitative and
// quantitative evaluation of 8 clear sky models"):
//     L(d) = E * Int_0^tmax  sum_i beta_i(h) P_i(cos theta)
//                 * exp(-tau_view(0..t) - tau_sun(h(t))) dt
// beta_R = (5.8, 13.5, 33.1) e-3 /km (680/550/440 nm), H_R = 8 km;
// beta_M = 21e-3 /km x haze (extinction 1.1 x scattering), H_M = 1.2 km;
// ozone absorption (0.650, 1.881, 0.085) e-3 /km, tent profile around 25 km
// (it makes twilight zeniths blue and horizons red); Rayleigh phase
// 3/(16 pi)(1 + c^2), Mie phase Henyey-Greenstein (1941) with g = 0.76.
// tau_sun(h) = (beta_R H_R e^{-h/H_R} + beta_M' H_M e^{-h/H_M} + ozone column
// above h) * m(e_sun), with the relative air mass of Kasten & Young (1989)
//     m(e) = 1 / (sin e + 0.50572 (e_deg + 6.07995)^-1.6364).
layout(push_constant) uniform PC {
    mat4 invViewProj;  // inverse of a perspective projection * view rotation (camera-relative)
    vec4 sun;          // xyz: direction to the sun (or moon), w: scene (1 sunset, 2 fjord, 3 night)
    vec4 unused0;
    vec4 unused1;
    vec4 tint;         // rgb: exposure, a: aerosol (haze) factor
} pc;
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265358979;
const float R_EARTH = 6371.0, R_TOP = 6431.0;           // km
const float H_R = 8.0, H_M = 1.2;                       // scale heights, km
const vec3 BETA_R = vec3(5.8e-3, 13.5e-3, 33.1e-3);     // /km
const float BETA_M = 21e-3;                             // /km
const vec3 OZONE = vec3(0.650e-3, 1.881e-3, 0.085e-3);  // /km
const float G_MIE = 0.76;
const int STEPS = 12;

float airmass(float sinh_) {
    float h = max(degrees(asin(clamp(sinh_, -1.0, 1.0))), -0.5);
    return 1.0 / (max(sinh_, 0.0) + 0.50572 * pow(h + 6.07995, -1.6364));
}
float phaseR(float c) { return 3.0 / (16.0 * PI) * (1.0 + c * c); }
float phaseM(float c) {
    float g2 = G_MIE * G_MIE;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * G_MIE * c, 1.5));
}
float ozoneDensity(float h) { return max(0.0, 1.0 - abs(h - 25.0) / 15.0); }
float ozoneColumnAbove(float h) { return 15.0 * (h < 10.0 ? 1.0 : max(0.0, (40.0 - h) / 30.0)); }  // km-equivalents

// transmittance of the whole atmosphere towards the sun, from the ground
vec3 sunTransmittance(vec3 s) {
    float haze = pc.tint.a;
    return exp(-(BETA_R * H_R + BETA_M * haze * 1.1 * H_M + OZONE * ozoneColumnAbove(0.0)) * airmass(s.z));
}

vec3 sky(vec3 d, vec3 s, float E) {
    float haze = pc.tint.a;
    float mu = max(d.z, 0.0);
    float tmax = -R_EARTH * mu + sqrt(R_EARTH * R_EARTH * mu * mu + R_TOP * R_TOP - R_EARTH * R_EARTH);
    float c = dot(d, s), ms = airmass(s.z);
    float pr = phaseR(c), pm = phaseM(c);
    vec3 L = vec3(0.0), tauv = vec3(0.0);
    float t0 = 0.0;
    for (int i = 1; i <= STEPS; ++i) {
        // exponential spacing: dense near the ground where the air is
        float t1 = tmax * (exp(3.0 * float(i) / float(STEPS)) - 1.0) / (exp(3.0) - 1.0);
        float t = 0.5 * (t0 + t1), dt = t1 - t0;
        t0 = t1;
        float h = sqrt(t * t + R_EARTH * R_EARTH + 2.0 * t * R_EARTH * mu) - R_EARTH;
        float rr = exp(-h / H_R), rm = exp(-h / H_M);
        vec3 ext = BETA_R * rr + BETA_M * haze * 1.1 * rm + OZONE * ozoneDensity(h);
        tauv += ext * dt * 0.5;
        vec3 taus = (BETA_R * H_R * rr + BETA_M * haze * 1.1 * H_M * rm + OZONE * ozoneColumnAbove(h)) * ms;
        L += (BETA_R * rr * pr + BETA_M * haze * rm * pm) * exp(-tauv - taus) * dt;
        tauv += ext * dt * 0.5;
    }
    L *= E;
    // sun disk (0.27 deg radius), attenuated along its path
    float sd = smoothstep(cos(radians(0.27)), cos(radians(0.22)), c);
    L += sd * E * 40.0 * sunTransmittance(s);
    return L;
}

// ---------------------------------------------------------------- noise
float hash11(float x) { return fract(sin(x * 127.1) * 43758.5453); }
float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise1(float x) {
    float i = floor(x), f = fract(x);
    float u = f * f * (3.0 - 2.0 * f);
    return mix(hash11(i), hash11(i + 1.0), u);
}
float vnoise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);  // quintic fade (Perlin 2002): C2, no cell edges
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), u.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), u.x), u.y);
}
float fbm2(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { v += a * vnoise2(p); p = p * 2.07 + vec2(5.2, 1.3); a *= 0.5; }
    return v;
}
// ridged fBm of the azimuth: mountain silhouettes (elevation angle in radians)
float ridge(float az, float seed, float freq) {
    float h = 0.0, a = 0.5, f = freq;
    for (int i = 0; i < 5; ++i) {
        float n = 1.0 - abs(vnoise1(az * f + seed) * 2.0 - 1.0);
        h += a * n * n;
        f *= 2.03, a *= 0.5;
    }
    return h;
}

// Mountains as horizon-elevation profiles, nearest layer first (it occludes).
// Fjord walls follow the valley geometry: a wall of height H parallel to the
// valley axis (here the X axis) at lateral distance w, seen at azimuth theta
// from the axis, is w/|sin theta| away, so its elevation is
//     el(theta) = atan((H/w) |sin theta|),
// steep at the sides and opening towards the far end of the fjord, where a
// lower snow-capped range closes the view. Ridged noise breaks the profile.
// The sunset scene only has a low, distant coastline.
float profile(int layer, float az, float scene) {
    float sa = abs(sin(az));
    if (scene < 1.5) {                    // sunset: distant coast, two layers
        if (layer == 0) return 0.0;
        return (layer == 1 ? 0.018 : 0.010) * ridge(az, 11.0 * float(layer), 2.0 + float(layer)) * smoothstep(0.2, 0.9, vnoise1(az * 1.3 + 4.0 * float(layer)));
    }
    if (layer == 0) return atan(1.25 * sa) * (0.62 + 0.45 * ridge(az, 3.0, 6.0));   // near walls
    if (layer == 1) return atan(0.55 * sa) * (0.70 + 0.50 * ridge(az, 23.0, 9.0)) + 0.012;  // second ridge line
    return 0.035 + 0.055 * ridge(az, 41.0, 7.0);                                    // far end, snow
}

vec4 mountains(vec3 d, vec3 s, float E, float scene) {
    float az = atan(d.y, d.x);
    float el = asin(clamp(d.z, -1.0, 1.0));
    for (int L = 0; L < 3; ++L) {
        float h = profile(L, az, scene);
        if (el < h && el > -0.002) {
            // only for covered pixels: horizon radiance (haze color) and zenith radiance (ambient)
            vec3 horizon = sky(normalize(vec3(d.xy, 0.03)), s, E);
            vec3 ambient = sky(vec3(0.0, 0.0, 1.0), s, E);
            vec3 n;
            vec3 albedo;
            float snow;
            // albedos: conifer forest ~0.08 (green), granite ~0.20, old snow ~0.75
            const vec3 FOREST = vec3(0.05, 0.09, 0.04), ROCK = vec3(0.20, 0.19, 0.18);
            if (L == 0 && scene > 1.5) {
                // near wall as a surface: lateral distance w = 1 km, so a point seen at
                // (az, el) lies at x = cot(az) km along the valley and z = tan(el)/|sin az| km high
                float sa = max(abs(sin(az)), 0.05);
                vec2 q = vec2(cos(az) / sa, tan(el) / sa);          // km
                float f = fbm2(q * vec2(3.0, 5.0));
                float fx = fbm2(q * vec2(3.0, 5.0) + vec2(0.02, 0.0)) - f;
                float fz = fbm2(q * vec2(3.0, 5.0) + vec2(0.0, 0.02)) - f;
                vec3 out_ = vec3(0.0, -sign(sin(az)), 0.0);           // the wall faces the valley axis
                n = normalize(out_ + vec3(0.0, 0.0, 0.55) + vec3(-fx, 0.0, -fz) * 30.0 + vec3(1, 0, 0) * (f - 0.5) * 0.8);
                float treeLine = 0.35 + 0.15 * f, snowLine = 0.95 + 0.2 * f;   // km
                albedo = mix(FOREST, ROCK, smoothstep(treeLine - 0.05, treeLine + 0.05, q.y)) * (0.7 + 0.6 * f);
                snow = smoothstep(snowLine, snowLine + 0.03, q.y);
            } else {
                float e = 0.02;   // low-frequency slope of the profile
                float dh = (profile(L, az + e, scene) - profile(L, az - e, scene)) / (2.0 * e);
                vec3 side = vec3(-sin(az), cos(az), 0.0);
                n = normalize(vec3(-d.xy, 0.0) - side * dh * 1.5 + vec3(0.0, 0.0, 0.45));
                float detail = 0.75 + 0.5 * fbm2(vec2(az * 60.0, el * 90.0));
                albedo = mix(FOREST, ROCK, smoothstep(0.35 * h, 0.55 * h, el)) * detail;
                snow = scene > 1.5 ? smoothstep(0.78 * h, 0.80 * h, el + 0.012 * vnoise1(az * 60.0)) : 0.0;
            }
            float lit = max(dot(n, s), 0.0);
            vec3 col = mix(albedo, vec3(0.75), snow);
            // Lambert: radiance = albedo/pi * (E T_sun cos + pi * L_sky_ambient)
            col = col / PI * (E * sunTransmittance(s) * lit + PI * ambient);
            // aerial perspective (Beer-Lambert, haze 0.04 /km): layers at ~2, 6, 25 km
            float distKm = L == 0 ? (scene > 1.5 ? 1.0 / max(abs(sin(az)), 0.05) : 2.0) : L == 1 ? 6.0 : 25.0;
            float T = exp(-0.04 * pc.tint.a * distKm);
            return vec4(mix(horizon, col, T), 1.0);
        }
    }
    return vec4(0.0);
}

vec3 stars(vec3 d) {
    // cells on a latitude/longitude grid; one star per cell at most
    vec2 sp = vec2(atan(d.y, d.x), asin(clamp(d.z, -1.0, 1.0))) * 180.0;
    vec2 cell = floor(sp);
    float h = hash21(cell);
    if (h < 0.92) return vec3(0.0);
    vec2 pos = cell + vec2(hash21(cell + 7.1), hash21(cell + 3.7));
    float r = length(sp - pos);
    float b = pow(hash21(cell + 1.3), 8.0) * 1.5 + 0.15;
    return vec3(0.9, 0.92, 1.0) * b * smoothstep(0.35, 0.0, r) * smoothstep(0.0, 0.15, d.z);
}

vec3 environment(vec3 d, float scene, bool reflected) {
    vec3 s = normalize(pc.sun.xyz);
    bool night = scene > 2.5;
    const float E = 1.0;  // the exposure (pc.tint.rgb) sets absolute brightness
    vec3 L = sky(d, s, E);
    if (night) {
        L += vec3(0.0004, 0.0006, 0.0014);                            // airglow floor
        if (!reflected) L += stars(d) * 0.25;
        float c = dot(d, s);
        L += vec3(0.9, 0.92, 0.95) * smoothstep(cos(radians(0.9)), cos(radians(0.8)), c) * 0.08;  // moon disk
    }
    vec4 m = mountains(d, s, E, scene);
    return mix(L, m.rgb, m.a);
}

vec3 tonemap(vec3 x) {  // ACES fit (Narkowicz 2015), then sRGB-ish gamma
    x *= pc.tint.rgb;
    x = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
    return pow(x, vec3(1.0 / 2.2));
}

void main() {
    vec4 a = pc.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec4 b = pc.invViewProj * vec4(vNdc, 0.5, 1.0);
    vec3 d = normalize(b.xyz / b.w - a.xyz / a.w);
    float scene = pc.sun.w;
    vec3 col;
    if (d.z >= 0.0) {
        col = environment(d, scene, false);
    } else {
        // water: mirror direction with ripples (normal perturbed by two noise
        // octaves on the water plane), Schlick Fresnel with F0 = 0.02 (n = 1.33)
        float t = 1.0 / max(-d.z, 1e-3);                  // distance to the plane (units of its depth)
        vec2 w = d.xy * t;
        float amp = 0.035 / (1.0 + 0.02 * t);             // ripples fade with distance (filtering)
        vec2 g = vec2(vnoise2(w * 3.0) - 0.5, vnoise2(w * 3.0 + 9.1) - 0.5) + 0.5 * vec2(vnoise2(w * 11.0) - 0.5, vnoise2(w * 11.0 + 4.3) - 0.5);
        vec3 n = normalize(vec3(g * amp, 1.0));
        vec3 r = reflect(d, n);
        r.z = abs(r.z);
        float cosi = clamp(-dot(d, n), 0.0, 1.0);
        float F = 0.02 + 0.98 * pow(1.0 - cosi, 5.0);
        // body color of fjord water: little upwelling light, blue-green (albedo ~0.02)
        vec3 deep = vec3(0.004, 0.010, 0.012) * (scene > 2.5 ? 0.1 : 1.0);
        col = mix(deep, environment(r, scene, true), F);
    }
    outColor = vec4(tonemap(col), 1.0);
}

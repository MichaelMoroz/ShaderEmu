/*
 * A fractal of an iterated function system, ray marched. A point of space is folded by the
 * three mirrors of a tetrahedron, turned, and scaled away from a corner, eight times over: what
 * is left says how far the point was from the Sierpinski pyramid those maps make. The machine
 * compiles this when the program starts (hlsl.h) and runs it on its lanes, a pixel each.
 * Change it and Run the C program again.
 */
float3 eye, forward, right, up;	// the camera, from the program
float2 size;			// the picture in pixels
float3x3 turn;			// what each fold turns
float time;

static const int FOLDS = 8;
static const float SCALE = 2.0;
static const float3 LIGHT = float3(0.48, 0.70, -0.53);

float pyramid(float3 p)
{
	for (int i = 0; i < FOLDS; i++) {
		if (p.x + p.y < 0) p.xy = -p.yx;
		if (p.x + p.z < 0) p.xz = -p.zx;
		if (p.y + p.z < 0) p.zy = -p.yz;
		p = mul(turn, p) * SCALE - (SCALE - 1.0);
	}
	return (length(p) - 1.2) * pow(SCALE, -(float)FOLDS);
}

float3 sky(float3 rd)
{
	return lerp(float3(0.42, 0.50, 0.66), float3(0.07, 0.08, 0.14), saturate(0.5 - 0.7 * rd.y));
}

float4 main(float4 pos : SV_Position) : SV_Target
{
	float2 uv = (pos.xy - 0.5 * size) / size.y;
	float3 rd = normalize(forward * 1.7 + right * uv.x - up * uv.y);

	// the pyramid lies inside a ball of radius 1.8 round the middle: march there only
	float b = dot(eye, rd), h = b * b - dot(eye, eye) + 3.24;
	if (h < 0) return float4(sky(rd), 1);
	h = sqrt(h);
	float t = max(-b - h, 0.0), far = -b + h;

	int i;
	for (i = 0; i < 90; i++) {
		float d = pyramid(eye + rd * t);
		if (d < 0.0004 * t || t > far) break;
		t += d;
	}
	if (t > far) return float4(sky(rd), 1);

	// the surface's normal from four distances round the point
	float3 p = eye + rd * t;
	float e = 0.0004 * t;
	float3 n = normalize(float3(1, -1, -1) * pyramid(p + float3(1, -1, -1) * e) +
			     float3(-1, -1, 1) * pyramid(p + float3(-1, -1, 1) * e) +
			     float3(-1, 1, -1) * pyramid(p + float3(-1, 1, -1) * e) +
			     float3(1, 1, 1) * pyramid(p + float3(1, 1, 1) * e));

	float ao = 1.0 - i / 90.0;
	float lit = saturate(dot(n, LIGHT));
	float3 base = 0.55 + 0.45 * cos(float3(0.0, 0.7, 1.6) + 2.2 * length(p) + 0.3 * time);
	float3 col = base * (0.18 + 0.82 * lit) * ao * ao;
	col += 0.25 * pow(saturate(dot(reflect(rd, n), LIGHT)), 12.0) * ao;
	col = lerp(col, sky(rd), saturate((t - 3.0) / 6.0));
	return float4(sqrt(col), 1);
}

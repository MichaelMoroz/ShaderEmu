using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

// The room's furniture and lighting. Everything is static geometry lit by baked lights and
// emissive surfaces; light probes carry that light to players. "ShaderEmu/Bake lighting"
// bakes it, and has to be run again after "Build world".
public static partial class ShaderEmuBuilder
{
    static GameObject Prim(Transform parent, PrimitiveType type, string name, Vector3 centre, Vector3 size, Material material,
                           bool collide = false)
    {
        GameObject go = GameObject.CreatePrimitive(type);
        go.name = name;
        go.transform.SetParent(parent, false);
        go.transform.localPosition = centre;
        // Unity's cylinder is two units tall
        go.transform.localScale = type == PrimitiveType.Cylinder ? new Vector3(size.x, size.y * 0.5f, size.z) : size;
        go.GetComponent<MeshRenderer>().sharedMaterial = material;
        if (!collide) Object.DestroyImmediate(go.GetComponent<Collider>());
        go.isStatic = true;
        return go;
    }

    // A box with rounded edges, as a mesh of its own size (a scaled one would squash the corners).
    static Mesh RoundedMesh(Vector3 size, float radius)
    {
        radius = Mathf.Min(radius, Mathf.Min(size.x, Mathf.Min(size.y, size.z)) * 0.5f - 0.001f);
        Directory.CreateDirectory(Generated + "/Meshes");
        string path = string.Format("{0}/Meshes/Rounded_{1}_{2}_{3}_{4}.asset", Generated, Mathf.RoundToInt(size.x * 1000),
                                    Mathf.RoundToInt(size.y * 1000), Mathf.RoundToInt(size.z * 1000), Mathf.RoundToInt(radius * 1000));
        return LoadOrCreate(path, () =>
        {
            const int segments = 5;
            Vector3 half = size * 0.5f, core = half - Vector3.one * radius;
            List<Vector3> vertices = new List<Vector3>(), normals = new List<Vector3>();
            List<Vector2> uvs = new List<Vector2>();
            List<int> triangles = new List<int>();
            for (int face = 0; face < 6; face++)
            {
                int a = face / 2, b = (a + 1) % 3, c = (a + 2) % 3;
                float sign = face % 2 == 0 ? 1f : -1f;
                // grid lines crowd into the rounded strips at both ends
                List<float> us = new List<float>(), vs = new List<float>();
                for (int k = 0; k <= segments; k++) { us.Add(-half[b] + radius * k / segments); vs.Add(-half[c] + radius * k / segments); }
                for (int k = 0; k <= segments; k++) { us.Add(half[b] - radius + radius * k / segments); vs.Add(half[c] - radius + radius * k / segments); }
                int first = vertices.Count, n = us.Count;
                foreach (float v in vs)
                    foreach (float u in us)
                    {
                        Vector3 p = Vector3.zero;
                        p[a] = sign * half[a];
                        p[b] = u;
                        p[c] = v;
                        Vector3 inner = new Vector3(Mathf.Clamp(p.x, -core.x, core.x), Mathf.Clamp(p.y, -core.y, core.y), Mathf.Clamp(p.z, -core.z, core.z));
                        Vector3 normal = (p - inner).normalized;
                        vertices.Add(inner + normal * radius);
                        normals.Add(normal);
                        uvs.Add(new Vector2(u / size[b] + 0.5f, v / size[c] + 0.5f));
                    }
                for (int y = 0; y < n - 1; y++)
                    for (int x = 0; x < n - 1; x++)
                    {
                        int i0 = first + y * n + x, i1 = i0 + 1, i2 = i0 + n, i3 = i2 + 1;
                        // wound so that the face looks outward
                        Vector3 facing = Vector3.Cross(vertices[i1] - vertices[i0], vertices[i2] - vertices[i0]);
                        bool flip = Vector3.Dot(facing, normals[i0] + normals[i3]) < 0;
                        if (flip) triangles.AddRange(new[] { i0, i2, i1, i1, i2, i3 });
                        else triangles.AddRange(new[] { i0, i1, i2, i1, i3, i2 });
                    }
            }
            Mesh mesh = new Mesh { name = "Rounded" };
            mesh.SetVertices(vertices);
            mesh.SetNormals(normals);
            mesh.SetUVs(0, uvs);
            mesh.SetTriangles(triangles, 0);
            mesh.RecalculateBounds();
            Unwrapping.GenerateSecondaryUVSet(mesh);   // for the lightmap
            return mesh;
        });
    }

    static GameObject RBox(Transform parent, string name, Vector3 centre, Vector3 size, Material material, float radius, bool collide = false)
    {
        GameObject go = new GameObject(name, typeof(MeshFilter), typeof(MeshRenderer));
        go.transform.SetParent(parent, false);
        go.transform.localPosition = centre;
        go.GetComponent<MeshFilter>().sharedMesh = RoundedMesh(size, radius);
        go.GetComponent<MeshRenderer>().sharedMaterial = material;
        if (collide) go.AddComponent<BoxCollider>().size = size;
        go.isStatic = true;
        return go;
    }

    const float WindowWide = 3.6f, WindowHigh = 1.5f, WindowY = 1.75f, WindowZ = -1.6f;

    static Light BakedLight(Transform parent, string name, Vector3 at, Color colour, float intensity, float range)
    {
        Light light = new GameObject(name).AddComponent<Light>();
        light.transform.SetParent(parent, false);
        light.transform.localPosition = at;
        light.type = LightType.Point;
        light.color = colour;
        light.intensity = intensity;
        light.range = range;
        light.shadows = LightShadows.Soft;
        light.shadowRadius = 0.25f;
        light.lightmapBakeType = LightmapBakeType.Baked;
        // On only while the lighting is baked: without baked data Unity draws such a light in
        // real time, shadows and all, which a headset cannot afford.
        light.enabled = false;
        return light;
    }

    // Letters for the poster, 5 x 7, top row first.
    static readonly Dictionary<char, string[]> Glyphs = new Dictionary<char, string[]>
    {
        { 'R', new[] { "1111.", "1...1", "1...1", "1111.", "1.1..", "1..1.", "1...1" } },
        { 'V', new[] { "1...1", "1...1", "1...1", "1...1", ".1.1.", ".1.1.", "..1.." } },
        { '3', new[] { "1111.", "....1", "....1", ".111.", "....1", "....1", "1111." } },
        { '2', new[] { ".111.", "1...1", "....1", "...1.", "..1..", ".1...", "11111" } },
        { 'I', new[] { "11111", "..1..", "..1..", "..1..", "..1..", "..1..", "11111" } },
        { 'M', new[] { "1...1", "11.11", "1.1.1", "1.1.1", "1...1", "1...1", "1...1" } },
        { 'A', new[] { ".111.", "1...1", "1...1", "11111", "1...1", "1...1", "1...1" } },
    };

    static bool Lettering(string text, float u, float v, float left, float top, float cell)
    {
        // v runs up in a texture; rows of the glyphs run down
        float x = (u - left) / cell, y = (top - v) / cell;
        if (x < 0 || y < 0 || y >= 7) return false;
        int letter = (int)(x / 6f), column = (int)x % 6;
        if (letter >= text.Length || column >= 5) return false;
        return Glyphs[text[letter]][(int)y][column] == '1';
    }

    // A quad of the city's mesh, wound to face along `normal`. Corners go round it; `shade`'s
    // alpha is the haze there.
    static void CityQuad(List<Vector3> vertices, List<Vector2> uvs, List<Color> colours, List<int> triangles,
                         Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector2 ua, Vector2 ub, Vector2 uc, Vector2 ud,
                         Vector3 normal, Color shade)
    {
        int first = vertices.Count;
        vertices.AddRange(new[] { a, b, c, d });
        uvs.AddRange(new[] { ua, ub, uc, ud });
        for (int i = 0; i < 4; i++) colours.Add(shade);
        bool flip = Vector3.Dot(Vector3.Cross(b - a, c - a), normal) < 0;
        if (flip) triangles.AddRange(new[] { first, first + 2, first + 1, first, first + 3, first + 2 });
        else triangles.AddRange(new[] { first, first + 1, first + 2, first, first + 2, first + 3 });
    }

    static Mesh CityMesh(string name, List<Vector3> vertices, List<Vector2> uvs, List<Color> colours, List<int> triangles)
    {
        AssetDatabase.DeleteAsset(Generated + "/" + name + ".asset");
        Mesh mesh = new Mesh { name = name, indexFormat = IndexFormat.UInt32 };
        mesh.SetVertices(vertices);
        mesh.SetUVs(0, uvs);
        mesh.SetColors(colours);
        mesh.SetTriangles(triangles, 0);
        mesh.RecalculateBounds();
        AssetDatabase.CreateAsset(mesh, Generated + "/" + name + ".asset");
        return mesh;
    }

    static void CityObject(Transform parent, string name, Mesh mesh, Material material)
    {
        GameObject go = new GameObject(name, typeof(MeshFilter), typeof(MeshRenderer));
        go.transform.SetParent(parent, false);
        go.GetComponent<MeshFilter>().sharedMesh = mesh;
        MeshRenderer renderer = go.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = material;
        renderer.shadowCastingMode = ShadowCastingMode.Off;
        renderer.receiveShadows = false;
        renderer.lightProbeUsage = LightProbeUsage.Off;
        renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
    }

    // The city outside the window, which is 42 m above its streets: one building a block, as
    // two meshes (towers, streets) of plain unlit geometry. `window` is the opening's centre.
    static void City(Transform decor, Vector3 window)
    {
        const float block = 16f, above = 42f, storey = 3.3f, bay = 2.4f;
        const int across = 30, rows = 40, cells = 16;
        System.Random random = new System.Random(11);
        System.Func<float, float, float> rand = (a, b) => a + (float)random.NextDouble() * (b - a);

        // windows: a tile of 16 x 16, some lit warm, a few cool, most dark
        Color[] lit = new Color[cells * cells];
        for (int i = 0; i < lit.Length; i++)
        {
            double chance = random.NextDouble();
            Color warm = random.NextDouble() < 0.25 ? new Color(0.65f, 0.85f, 1f) : new Color(1f, 0.72f, 0.38f);
            lit[i] = chance > 0.62 ? warm * rand(0.55f, 1f) : new Color(0.05f, 0.06f, 0.09f);
        }
        Texture2D windowsTex = Pattern("CityWindows", 256, (u, v) =>
        {
            float x = u * cells, y = v * cells;
            float fx = x - Mathf.Floor(x), fy = y - Mathf.Floor(y);
            bool glass = fx > 0.18f && fx < 0.82f && fy > 0.25f && fy < 0.80f;
            Color c = glass ? lit[((int)y % cells) * cells + (int)x % cells] : new Color(0.07f, 0.075f, 0.10f);
            c.a = 1f;
            return c;
        });
        Texture2D streetTex = Pattern("CityStreet", 128, (u, v) =>
        {
            // one block: dark ground, a lighter road round it, lamps along the road
            float gx = Mathf.Abs(u - 0.5f), gy = Mathf.Abs(v - 0.5f);
            bool road = Mathf.Max(gx, gy) > 0.42f;
            float lx = Mathf.Abs(Mathf.Repeat(u * 4f, 1f) - 0.5f), ly = Mathf.Abs(Mathf.Repeat(v * 4f, 1f) - 0.5f);
            float lamp = road ? Mathf.Clamp01(1f - Mathf.Sqrt(lx * lx + ly * ly) * 7f) : 0f;
            Color c = road ? new Color(0.20f, 0.17f, 0.14f) : new Color(0.07f, 0.07f, 0.08f);
            return c + new Color(1f, 0.7f, 0.35f) * lamp;
        });
        Material towerMat = Mat("City", "ShaderEmu/City");
        towerMat.mainTexture = windowsTex;
        Material streetMat = Mat("CityStreet", "ShaderEmu/City");
        streetMat.mainTexture = streetTex;

        Transform city = new GameObject("City").transform;
        city.SetParent(decor, false);
        Vector3 origin = new Vector3(window.x, window.y - above, window.z);   // street level, under the window
        System.Func<Vector3, float> haze = p => 1f - Mathf.Exp(-Vector3.Distance(p, window) / 900f);

        List<Vector3> vertices = new List<Vector3>();
        List<Vector2> uvs = new List<Vector2>();
        List<Color> colours = new List<Color>();
        List<int> triangles = new List<int>();
        for (int r = 2; r < rows; r++)   // nothing in the first two rows of blocks
            for (int a = -across; a < across; a++)
            {
                float seed = rand(0f, 1f), side = block * rand(0.26f, 0.40f), extra = rand(0f, 30f);
                Vector2 offset = new Vector2(random.Next(cells), random.Next(cells)) / cells;
                if (seed < 0.22f) continue;   // an empty lot
                float tall = 14f + 80f * seed * seed + (seed > 0.8f ? extra : 0f);
                Vector3 centre = origin + new Vector3((r + 0.5f) * block, 0, (a + 0.5f) * block);
                float x0 = centre.x - side, x1 = centre.x + side, z0 = centre.z - side, z1 = centre.z + side, y0 = origin.y, y1 = origin.y + tall;
                float h = haze(new Vector3(x0, window.y, centre.z));
                float us = 2f * side / bay / cells, vs = tall / storey / cells;
                Vector2 u00 = offset, u10 = offset + new Vector2(us, 0), u11 = offset + new Vector2(us, vs), u01 = offset + new Vector2(0, vs);
                // the three walls the room can see, and the roof (dark: a spot of wall between windows)
                CityQuad(vertices, uvs, colours, triangles, new Vector3(x0, y0, z0), new Vector3(x0, y0, z1), new Vector3(x0, y1, z1), new Vector3(x0, y1, z0),
                         u00, u10, u11, u01, Vector3.left, new Color(1f, 1f, 1f, h));
                CityQuad(vertices, uvs, colours, triangles, new Vector3(x0, y0, z0), new Vector3(x1, y0, z0), new Vector3(x1, y1, z0), new Vector3(x0, y1, z0),
                         u00, u10, u11, u01, Vector3.back, new Color(0.6f, 0.6f, 0.6f, h));
                CityQuad(vertices, uvs, colours, triangles, new Vector3(x0, y0, z1), new Vector3(x1, y0, z1), new Vector3(x1, y1, z1), new Vector3(x0, y1, z1),
                         u00, u10, u11, u01, Vector3.forward, new Color(0.6f, 0.6f, 0.6f, h));
                CityQuad(vertices, uvs, colours, triangles, new Vector3(x0, y1, z0), new Vector3(x1, y1, z0), new Vector3(x1, y1, z1), new Vector3(x0, y1, z1),
                         Vector2.zero, Vector2.zero, Vector2.zero, Vector2.zero, Vector3.up, new Color(0.5f, 0.5f, 0.5f, h));
            }
        CityObject(city, "Towers", CityMesh("CityTowers", vertices, uvs, colours, triangles), towerMat);

        vertices.Clear();
        uvs.Clear();
        colours.Clear();
        triangles.Clear();
        for (int r = 0; r < rows + 8; r++)
            for (int a = -across - 8; a < across + 8; a++)
            {
                Vector3 p = origin + new Vector3(r * block, 0, a * block);
                Vector3 q = p + new Vector3(block, 0, block);
                float h = haze(new Vector3(p.x, origin.y, p.z));
                CityQuad(vertices, uvs, colours, triangles, p, new Vector3(q.x, p.y, p.z), q, new Vector3(p.x, p.y, q.z),
                         new Vector2(0, 0), new Vector2(1, 0), new Vector2(1, 1), new Vector2(0, 1), Vector3.up, new Color(1f, 1f, 1f, h));
            }
        CityObject(city, "Streets", CityMesh("CityStreets", vertices, uvs, colours, triangles), streetMat);
    }

    // The night sky as a panorama for the skybox: dark above, the city's glow on the haze at
    // the horizon, stars, and a moon on the window's side of the room.
    static Material NightSky()
    {
        const int wide = 2048, high = 1024;
        string path = Root + "/Textures/NightSky.png";
        Color[] pixels = new Color[wide * high];
        Vector3 moon = new Vector3(0.84f, 0.42f, 0.35f).normalized;
        for (int y = 0; y < high; y++)
            for (int x = 0; x < wide; x++)
            {
                // as Unity's panoramic skybox reads it: u = 0.5 looks along +x, v = 1 straight up
                float angle = (0.5f - (x + 0.5f) / wide) * 2f * Mathf.PI, up = Mathf.Cos((1f - (y + 0.5f) / high) * Mathf.PI);
                float flat = Mathf.Sqrt(Mathf.Max(0f, 1f - up * up));
                Vector3 d = new Vector3(Mathf.Cos(angle) * flat, up, Mathf.Sin(angle) * flat);
                Color c = Color.Lerp(new Color(0.10f, 0.13f, 0.26f), new Color(0.004f, 0.006f, 0.02f), Mathf.Clamp01(d.y * 1.6f + 0.1f));
                c += new Color(0.30f, 0.16f, 0.10f) * Mathf.Pow(Mathf.Clamp01(1f - Mathf.Abs(d.y) * 6f), 3f);
                if (d.y < 0) c = Color.Lerp(new Color(0.12f, 0.09f, 0.10f), new Color(0.01f, 0.01f, 0.015f), Mathf.Clamp01(-d.y * 3f));
                float m = Vector3.Dot(d, moon);
                if (m > 0.9985f) c = new Color(0.95f, 0.93f, 0.85f);
                else c += new Color(0.25f, 0.25f, 0.3f) * Mathf.Pow(Mathf.Clamp01(m), 60f);
                pixels[y * wide + x] = new Color(Mathf.LinearToGammaSpace(c.r), Mathf.LinearToGammaSpace(c.g), Mathf.LinearToGammaSpace(c.b), 1f);
            }
        System.Random random = new System.Random(5);
        for (int i = 0; i < 2600; i++)
        {
            // stars: evenly over the upper half of the sphere, most of them faint
            float v = 0.5f + 0.5f * Mathf.Asin((float)random.NextDouble()) / (Mathf.PI / 2f);
            int x = random.Next(wide), y = Mathf.Min(high - 1, (int)(v * high));
            float bright = Mathf.Pow((float)random.NextDouble(), 3f) * 0.9f + 0.1f;
            if (y < high * 0.52f) continue;
            pixels[y * wide + x] += new Color(bright, bright, bright * 1.05f, 0f);
        }
        Texture2D tex = new Texture2D(wide, high, TextureFormat.RGBA32, false);
        tex.SetPixels(pixels);
        File.WriteAllBytes(path, tex.EncodeToPNG());
        Object.DestroyImmediate(tex);
        AssetDatabase.ImportAsset(path);
        Material sky = Mat("NightSky", "Skybox/Panoramic");
        sky.SetTexture("_MainTex", AssetDatabase.LoadAssetAtPath<Texture2D>(path));
        sky.SetFloat("_Mapping", 1f);     // latitude and longitude
        sky.SetFloat("_ImageType", 0f);   // all the way round
        sky.SetFloat("_Exposure", 1f);
        return sky;
    }

    static void Decorate(Transform world, Transform room, float halfW, float halfD, float height,
                         Material woodMat, Material plasticMat, Material metalMat, Material ledMat, Material lampMat)
    {
        System.Random random = new System.Random(7);
        System.Func<float, float, float> rand = (a, b) => a + (float)random.NextDouble() * (b - a);

        // ---- materials
        Texture2D rugTex = Pattern("Rug", 512, (u, v) =>
        {
            float edge = Mathf.Min(Mathf.Min(u, 1 - u) * 1.4f, Mathf.Min(v, 1 - v));
            float weave = 0.92f + 0.08f * Mathf.Sin(u * 400f) * Mathf.Sin(v * 400f);
            Color c = edge < 0.03f ? new Color(0.75f, 0.68f, 0.5f) : edge < 0.06f ? new Color(0.12f, 0.16f, 0.26f)
                    : edge < 0.075f ? new Color(0.75f, 0.68f, 0.5f) : new Color(0.20f, 0.27f, 0.42f);
            float diamond = Mathf.Abs(Mathf.Repeat(u * 6f, 1f) - 0.5f) + Mathf.Abs(Mathf.Repeat(v * 4f, 1f) - 0.5f);
            if (edge >= 0.075f && diamond < 0.12f) c = new Color(0.62f, 0.34f, 0.26f);
            return c * weave;
        });
        Texture2D fabricTex = Pattern("Fabric", 256, (u, v) =>
        {
            float weave = 0.82f + 0.10f * Mathf.Sin(u * 250f) * Mathf.Sin(v * 250f) + (Noise(u, v, 12f) - 0.5f) * 0.2f;
            return new Color(weave, weave, weave, 1f);
        });
        Texture2D dieTex = Pattern("PosterDie", 512, (u, v) =>
        {
            // a made-up chip: blocks of memory and logic on a dark substrate
            if (Mathf.Min(Mathf.Min(u, 1 - u), Mathf.Min(v, 1 - v)) < 0.05f) return new Color(0.92f, 0.9f, 0.84f);
            float gx = u * 6f, gy = v * 8f;
            int cx = (int)gx, cy = (int)gy;
            float fx = gx - cx, fy = gy - cy;
            float seed = Mathf.PerlinNoise(cx * 3.7f + 0.5f, cy * 5.3f + 0.5f);
            if (fx < 0.06f || fy < 0.06f) return new Color(0.03f, 0.04f, 0.06f);
            float lines = seed > 0.55f ? (Mathf.Repeat(fy * 14f, 1f) > 0.5f ? 1f : 0f) : (Mathf.Repeat(fx * 10f + fy * 10f, 1f) > 0.5f ? 1f : 0f);
            Color tint = seed > 0.6f ? new Color(0.95f, 0.55f, 0.2f) : seed > 0.4f ? new Color(0.2f, 0.75f, 0.85f) : new Color(0.55f, 0.35f, 0.85f);
            return tint * (0.35f + 0.45f * lines);
        });
        Texture2D sunsetTex = Pattern("PosterGrid", 512, (u, v) =>
        {
            if (Mathf.Min(Mathf.Min(u, 1 - u), Mathf.Min(v, 1 - v)) < 0.05f) return new Color(0.06f, 0.05f, 0.08f);
            if (v > 0.45f)
            {
                Color sky = Color.Lerp(new Color(0.95f, 0.45f, 0.25f), new Color(0.12f, 0.05f, 0.30f), (v - 0.45f) / 0.5f);
                float d = Vector2.Distance(new Vector2(u, v), new Vector2(0.5f, 0.56f));
                bool sun = d < 0.2f && (v > 0.6f || Mathf.Repeat(v * 40f, 1f) > 0.4f);
                return sun ? new Color(1f, 0.85f, 0.35f) : sky;
            }
            // a floor of lines running to the horizon
            float depth = 0.45f - v, across = (u - 0.5f) / (depth + 0.02f);
            bool line = Mathf.Repeat(across * 0.9f, 1f) < 0.06f || Mathf.Repeat(0.6f / (depth + 0.02f), 1f) < 0.10f;
            return line ? new Color(0.95f, 0.3f, 0.8f) : new Color(0.07f, 0.03f, 0.14f);
        });
        Texture2D wordsTex = Pattern("PosterWords", 512, (u, v) =>
        {
            if (Mathf.Min(Mathf.Min(u, 1 - u), Mathf.Min(v, 1 - v)) < 0.05f) return new Color(0.92f, 0.9f, 0.84f);
            if (Lettering("RV32", u, v, 0.17f, 0.80f, 0.029f)) return new Color(0.98f, 0.75f, 0.2f);
            if (Lettering("IMA", u, v, 0.25f, 0.50f, 0.029f)) return new Color(0.3f, 0.8f, 0.9f);
            bool rule = v > 0.2f && v < 0.215f && u > 0.15f && u < 0.85f;
            return rule ? new Color(0.9f, 0.9f, 0.9f) : new Color(0.09f, 0.10f, 0.13f);
        });
        Material rugMat = Matte(Lit("Rug", Color.white, rugTex, 1f, 0.05f));
        Material sofaMat = Matte(Lit("Sofa", new Color(0.30f, 0.36f, 0.44f), fabricTex, 3f, 0.08f));
        Material cushionMat = Matte(Lit("Cushion", new Color(0.78f, 0.48f, 0.26f), fabricTex, 2f, 0.08f));
        Material potMat = Lit("Pot", new Color(0.70f, 0.42f, 0.30f), null, 1f, 0.2f);
        Material leafMat = Lit("Leaf", new Color(0.16f, 0.42f, 0.18f), null, 1f, 0.3f);
        Material cardboardMat = Matte(Lit("Cardboard", new Color(0.62f, 0.48f, 0.32f), fabricTex, 1f, 0.05f));
        Material whiteMat = Lit("White", new Color(0.88f, 0.88f, 0.86f), null, 1f, 0.4f);
        Material trimMat = Lit("Trim", new Color(0.93f, 0.92f, 0.90f), null, 1f, 0.15f);
        Material dieMat = Lit("PosterDie", Color.white, dieTex, 1f, 0.2f);
        Material sunsetMat = Lit("PosterGrid", Color.white, sunsetTex, 1f, 0.2f);
        Material wordsMat = Lit("PosterWords", Color.white, wordsTex, 1f, 0.2f);
        Material stripMat = Glow("Strip", new Color(0.25f, 1.1f, 1.6f));
        Material amberMat = Glow("Amber", new Color(1.8f, 0.9f, 0.15f));
        Material shadeMat = Glow("Shade", new Color(1.9f, 1.35f, 0.8f));
        Color[] spines =
        {
            new Color(0.55f, 0.16f, 0.14f), new Color(0.16f, 0.30f, 0.52f), new Color(0.20f, 0.42f, 0.26f),
            new Color(0.78f, 0.62f, 0.22f), new Color(0.36f, 0.22f, 0.44f), new Color(0.82f, 0.80f, 0.74f),
        };
        Material[] bookMats = new Material[spines.Length];
        for (int i = 0; i < spines.Length; i++) bookMats[i] = Lit("Book" + i, spines[i], null, 1f, 0.25f);

        Transform decor = new GameObject("Decor").transform;
        decor.SetParent(world, false);

        // ---- trim, beams, lamps
        float t = 0.012f;
        Box(decor, "Baseboard front", new Vector3(0, 0.05f, halfD - t), new Vector3(halfW * 2, 0.1f, 0.024f), trimMat, false);
        Box(decor, "Baseboard back", new Vector3(0, 0.05f, -halfD + t), new Vector3(halfW * 2, 0.1f, 0.024f), trimMat, false);
        Box(decor, "Baseboard left", new Vector3(-halfW + t, 0.05f, 0), new Vector3(0.024f, 0.1f, halfD * 2 - 0.05f), trimMat, false);
        Box(decor, "Baseboard right", new Vector3(halfW - t, 0.05f, 0), new Vector3(0.024f, 0.1f, halfD * 2 - 0.05f), trimMat, false);
        foreach (float z in new[] { -4.3f, 0f, 4.3f })
            Box(decor, "Beam", new Vector3(0, height - 0.09f, z), new Vector3(halfW * 2, 0.18f, 0.22f), woodMat, false);
        foreach (float z in new[] { -2.2f, 2.3f })
            for (int i = -1; i <= 1; i++)
            {
                Vector3 at = new Vector3(i * 2.7f, height, z);
                RBox(decor, "Lamp frame", at + new Vector3(0, -0.02f, 0), new Vector3(1.36f, 0.05f, 0.40f), metalMat, 0.02f);
                Box(decor, "Lamp", at + new Vector3(0, -0.045f, 0), new Vector3(1.26f, 0.012f, 0.30f), lampMat, false);
                BakedLight(decor, "Lamp light", at + new Vector3(0, -0.35f, 0), new Color(1f, 0.95f, 0.88f), 2.2f, 9f);
            }

        // ---- the wall of screens: light strips round the panel, cables to the tower
        float panelZ = halfD - 0.07f;
        Box(decor, "Strip top", new Vector3(0, 2.77f, panelZ), new Vector3(7.6f, 0.02f, 0.02f), stripMat, false);
        Box(decor, "Strip bottom", new Vector3(0, 0.80f, panelZ), new Vector3(7.6f, 0.02f, 0.02f), stripMat, false);
        Box(decor, "Cable duct", new Vector3(1.55f, 0.64f, halfD - 0.035f), new Vector3(0.06f, 0.26f, 0.03f), plasticMat, false);
        BakedLight(decor, "Screen glow", new Vector3(0, 1.8f, halfD - 1.0f), new Color(0.7f, 0.82f, 1f), 1.0f, 6f);

        // ---- server rack, right of the desk
        Vector3 rackAt = new Vector3(3.75f, 1.0f, halfD - 0.45f);
        Box(decor, "Rack", rackAt, new Vector3(0.7f, 2.0f, 0.8f), plasticMat);
        for (int u = 0; u < 9; u++)
        {
            float y = 0.25f + u * 0.19f;
            Box(decor, "Rack unit", new Vector3(rackAt.x, y, rackAt.z - 0.405f), new Vector3(0.6f, 0.15f, 0.012f), metalMat, false);
            for (int l = 0; l < 5; l++)
                if (random.NextDouble() < 0.8)
                    Box(decor, "Rack led", new Vector3(rackAt.x - 0.24f + l * 0.035f, y + 0.04f, rackAt.z - 0.413f),
                        new Vector3(0.014f, 0.014f, 0.004f), random.NextDouble() < 0.75 ? ledMat : amberMat, false);
        }

        // ---- sofa, rug and low table at the back
        Vector3 sofaAt = new Vector3(-0.4f, 0, -halfD + 0.55f);
        Box(decor, "Rug", new Vector3(-0.4f, 0.011f, -halfD + 1.75f), new Vector3(3.8f, 0.022f, 2.2f), rugMat, false);
        RBox(decor, "Sofa base", sofaAt + new Vector3(0, 0.24f, 0), new Vector3(2.3f, 0.30f, 0.9f), sofaMat, 0.08f, true);
        RBox(decor, "Sofa back", sofaAt + new Vector3(0, 0.62f, -0.34f), new Vector3(2.3f, 0.66f, 0.24f), sofaMat, 0.10f, true);
        foreach (float x in new[] { -1.08f, 1.08f })
            RBox(decor, "Sofa arm", sofaAt + new Vector3(x, 0.42f, 0), new Vector3(0.24f, 0.56f, 0.92f), sofaMat, 0.10f, true);
        for (int i = -1; i <= 1; i++)
            RBox(decor, "Sofa seat", sofaAt + new Vector3(i * 0.63f, 0.42f, 0.08f), new Vector3(0.62f, 0.16f, 0.72f), sofaMat, 0.07f);
        GameObject pillow = RBox(decor, "Pillow", sofaAt + new Vector3(-0.70f, 0.66f, -0.10f), new Vector3(0.42f, 0.42f, 0.14f), cushionMat, 0.06f);
        pillow.transform.localEulerAngles = new Vector3(-18f, 14f, 8f);
        GameObject pillow2 = RBox(decor, "Pillow", sofaAt + new Vector3(0.72f, 0.66f, -0.10f), new Vector3(0.40f, 0.40f, 0.14f), cushionMat, 0.06f);
        pillow2.transform.localEulerAngles = new Vector3(-16f, -20f, -6f);
        foreach (float x in new[] { -1.0f, 1.0f })
            foreach (float z in new[] { -0.33f, 0.33f })
                Prim(decor, PrimitiveType.Cylinder, "Sofa foot", sofaAt + new Vector3(x, 0.045f, z), new Vector3(0.06f, 0.09f, 0.06f), woodMat);

        Vector3 tableAt = new Vector3(-0.4f, 0, -halfD + 1.75f);
        RBox(decor, "Low table top", tableAt + new Vector3(0, 0.40f, 0), new Vector3(1.2f, 0.05f, 0.6f), woodMat, 0.024f, true);
        foreach (float x in new[] { -0.5f, 0.5f })
            foreach (float z in new[] { -0.22f, 0.22f })
                Prim(decor, PrimitiveType.Cylinder, "Low table leg", tableAt + new Vector3(x, 0.19f, z), new Vector3(0.04f, 0.38f, 0.04f), metalMat);
        Prim(decor, PrimitiveType.Cylinder, "Mug", tableAt + new Vector3(0.32f, 0.475f, 0.08f), new Vector3(0.08f, 0.10f, 0.08f), whiteMat);
        for (int i = 0; i < 3; i++)
        {
            GameObject book = RBox(decor, "Table book", tableAt + new Vector3(-0.28f, 0.44f + i * 0.032f, -0.04f),
                                   new Vector3(0.26f - i * 0.02f, 0.03f, 0.19f), bookMats[(i * 2 + 1) % bookMats.Length], 0.004f);
            book.transform.localEulerAngles = new Vector3(0, rand(-14f, 14f), 0);
        }

        // ---- a round table in the middle of the room, stools round it
        Vector3 roundAt = new Vector3(0, 0, -1.3f);
        Prim(decor, PrimitiveType.Cylinder, "Round rug", roundAt + new Vector3(0, 0.011f, 0), new Vector3(2.7f, 0.022f, 2.7f), rugMat);
        Prim(decor, PrimitiveType.Cylinder, "Round table top", roundAt + new Vector3(0, 0.74f, 0), new Vector3(1.5f, 0.05f, 1.5f), woodMat, true);
        Prim(decor, PrimitiveType.Cylinder, "Round table post", roundAt + new Vector3(0, 0.37f, 0), new Vector3(0.12f, 0.70f, 0.12f), metalMat, true);
        Prim(decor, PrimitiveType.Cylinder, "Round table foot", roundAt + new Vector3(0, 0.02f, 0), new Vector3(0.75f, 0.04f, 0.75f), metalMat);
        for (int i = 0; i < 4; i++)
        {
            float angle = (i * 90f + 45f) * Mathf.Deg2Rad;
            Vector3 at = roundAt + new Vector3(Mathf.Cos(angle), 0, Mathf.Sin(angle)) * 1.15f;
            Prim(decor, PrimitiveType.Cylinder, "Stool seat", at + new Vector3(0, 0.47f, 0), new Vector3(0.38f, 0.06f, 0.38f), cushionMat, true);
            Prim(decor, PrimitiveType.Cylinder, "Stool post", at + new Vector3(0, 0.23f, 0), new Vector3(0.05f, 0.44f, 0.05f), metalMat);
            Prim(decor, PrimitiveType.Cylinder, "Stool foot", at + new Vector3(0, 0.015f, 0), new Vector3(0.32f, 0.03f, 0.32f), metalMat);
        }
        // on it: a small plant, mugs, a laptop left open
        Prim(decor, PrimitiveType.Cylinder, "Table pot", roundAt + new Vector3(0, 0.83f, 0), new Vector3(0.16f, 0.13f, 0.16f), potMat);
        for (int i = 0; i < 6; i++)
        {
            float angle = i * 1.05f;
            GameObject leaf = Prim(decor, PrimitiveType.Sphere, "Table leaf",
                                   roundAt + new Vector3(Mathf.Cos(angle) * 0.07f, 0.95f + (i % 3) * 0.04f, Mathf.Sin(angle) * 0.07f),
                                   new Vector3(0.16f, 0.04f, 0.09f), leafMat);
            leaf.transform.localEulerAngles = new Vector3(rand(-20f, 20f), -angle * Mathf.Rad2Deg, -25f);
        }
        Prim(decor, PrimitiveType.Cylinder, "Mug", roundAt + new Vector3(0.42f, 0.815f, 0.22f), new Vector3(0.08f, 0.10f, 0.08f), whiteMat);
        Prim(decor, PrimitiveType.Cylinder, "Mug", roundAt + new Vector3(-0.30f, 0.815f, -0.38f), new Vector3(0.08f, 0.10f, 0.08f), cushionMat);
        Transform laptop = new GameObject("Laptop").transform;
        laptop.SetParent(decor, false);
        laptop.localPosition = roundAt + new Vector3(-0.38f, 0.765f, 0.30f);
        laptop.localEulerAngles = new Vector3(0, 140f, 0);
        RBox(laptop, "Laptop base", new Vector3(0, 0.008f, 0), new Vector3(0.32f, 0.016f, 0.22f), metalMat, 0.006f);
        GameObject lid = RBox(laptop, "Laptop lid", new Vector3(0, 0.105f, 0.135f), new Vector3(0.32f, 0.21f, 0.012f), metalMat, 0.005f);
        lid.transform.localEulerAngles = new Vector3(-18f, 0, 0);
        GameObject glow = Box(laptop, "Laptop screen", new Vector3(0, 0.105f, 0.127f), new Vector3(0.29f, 0.18f, 0.002f), stripMat, false);
        glow.transform.localEulerAngles = new Vector3(-18f, 0, 0);

        // ---- floor lamp by the sofa
        Vector3 lampAt = new Vector3(1.15f, 0, -halfD + 0.4f);
        Prim(decor, PrimitiveType.Cylinder, "Floor lamp foot", lampAt + new Vector3(0, 0.015f, 0), new Vector3(0.32f, 0.03f, 0.32f), metalMat);
        Prim(decor, PrimitiveType.Cylinder, "Floor lamp pole", lampAt + new Vector3(0, 0.8f, 0), new Vector3(0.03f, 1.6f, 0.03f), metalMat);
        Prim(decor, PrimitiveType.Cylinder, "Floor lamp shade", lampAt + new Vector3(0, 1.62f, 0), new Vector3(0.36f, 0.3f, 0.36f), shadeMat);
        BakedLight(decor, "Floor lamp light", lampAt + new Vector3(0, 1.35f, 0.25f), new Color(1f, 0.74f, 0.45f), 2.0f, 5f);

        // ---- bookshelf on the left wall
        const float shelfDepth = 0.32f, shelfWide = 2.0f, shelfHigh = 2.1f;
        Vector3 shelfAt = new Vector3(-halfW + shelfDepth / 2, 0, -2.6f);
        Box(decor, "Shelf back", shelfAt + new Vector3(-shelfDepth / 2 + 0.018f, shelfHigh / 2 - 0.005f, 0), new Vector3(0.036f, shelfHigh - 0.02f, shelfWide - 0.03f), woodMat, false);
        foreach (float z in new[] { -shelfWide / 2, shelfWide / 2 })
            Box(decor, "Shelf side", shelfAt + new Vector3(0, shelfHigh / 2, z), new Vector3(shelfDepth, shelfHigh, 0.03f), woodMat);
        for (int s = 0; s < 6; s++)
        {
            float y = 0.05f + s * 0.40f;
            Box(decor, "Shelf", shelfAt + new Vector3(-0.004f, y, 0), new Vector3(shelfDepth - 0.012f, 0.03f, shelfWide - 0.03f), woodMat, s == 0 || s == 5);
            if (s == 5) break;
            float z = -shelfWide / 2 + 0.05f;
            while (z < shelfWide / 2 - 0.12f)
            {
                if (random.NextDouble() < 0.12) { z += rand(0.10f, 0.25f); continue; }   // a gap
                float thick = rand(0.025f, 0.06f), tall = rand(0.22f, 0.33f), deep = rand(0.18f, 0.25f);
                // no two spines in one plane, or a leaning book fights its neighbour for the same pixels
                float front = 0.12f - rand(0f, 0.035f);
                float lean = random.NextDouble() < 0.1 ? rand(6f, 12f) : 0f;
                float sine = Mathf.Sin(lean * Mathf.Deg2Rad);
                GameObject book = Box(decor, "Book", shelfAt + new Vector3(front - deep / 2, y + 0.016f + tall / 2 + thick * 0.5f * sine, z + thick / 2),
                                      new Vector3(deep, tall, thick), bookMats[random.Next(bookMats.Length)], false);
                book.transform.localEulerAngles = new Vector3(lean, 0, 0);
                z += thick + 0.004f + tall * sine;   // its top leans this way
            }
        }

        // ---- plants
        foreach (Vector3 at in new[] { new Vector3(-halfW + 0.45f, 0, 1.6f), new Vector3(2.05f, 0, -halfD + 0.45f), new Vector3(halfW - 0.45f, 0, 1.2f) })
        {
            Prim(decor, PrimitiveType.Cylinder, "Pot", at + new Vector3(0, 0.2f, 0), new Vector3(0.36f, 0.4f, 0.36f), potMat, true);
            Prim(decor, PrimitiveType.Cylinder, "Stem", at + new Vector3(0, 0.7f, 0), new Vector3(0.03f, 0.7f, 0.03f), leafMat);
            for (int i = 0; i < 9; i++)
            {
                float angle = i * 2.4f, reach = rand(0.08f, 0.26f), up = 0.65f + i * 0.07f;
                GameObject leaf = Prim(decor, PrimitiveType.Sphere, "Leaf",
                                       at + new Vector3(Mathf.Cos(angle) * reach, up, Mathf.Sin(angle) * reach),
                                       new Vector3(rand(0.22f, 0.34f), rand(0.05f, 0.09f), rand(0.12f, 0.18f)), leafMat);
                leaf.transform.localEulerAngles = new Vector3(rand(-25f, 25f), -angle * Mathf.Rad2Deg, rand(-30f, 10f));
            }
        }

        // ---- a wide window on the right wall: an opening (Build leaves it in the wall) onto the city
        const float windowWide = WindowWide, windowHigh = WindowHigh;
        Vector3 windowAt = new Vector3(halfW - 0.012f, WindowY, WindowZ);
        BoxCollider closed = new GameObject("Window (nobody climbs out)").AddComponent<BoxCollider>();
        closed.transform.SetParent(decor, false);
        closed.transform.localPosition = new Vector3(halfW + 0.1f, WindowY, WindowZ);
        closed.size = new Vector3(0.2f, windowHigh, windowWide);
        City(decor, new Vector3(halfW + 0.2f, WindowY, WindowZ));
        Box(decor, "Window top", windowAt + new Vector3(-0.03f, windowHigh / 2 + 0.035f, 0), new Vector3(0.08f, 0.07f, windowWide + 0.14f), trimMat, false);
        RBox(decor, "Window sill", windowAt + new Vector3(-0.08f, -windowHigh / 2 - 0.03f, 0), new Vector3(0.20f, 0.06f, windowWide + 0.2f), trimMat, 0.02f);
        for (int i = 0; i <= 3; i++)
            Box(decor, "Window post", windowAt + new Vector3(-0.03f, 0, (i / 3f - 0.5f) * (windowWide + 0.07f)),
                new Vector3(0.08f, windowHigh, 0.07f), trimMat, false);
        for (int i = -1; i <= 1; i += 2)
            BakedLight(decor, "Window light", windowAt + new Vector3(-0.7f, 0.1f, i * 0.9f), new Color(0.42f, 0.55f, 1f), 0.8f, 5f);

        // ---- door, posters, boxes on the back wall
        Vector3 doorAt = new Vector3(3.4f, 1.05f, -halfD + 0.02f);
        Box(decor, "Door", doorAt, new Vector3(0.95f, 2.1f, 0.04f), woodMat, false);
        Box(decor, "Door frame top", doorAt + new Vector3(0, 1.09f, 0), new Vector3(1.11f, 0.08f, 0.06f), trimMat, false);
        foreach (float x in new[] { -0.515f, 0.515f })
            Box(decor, "Door frame side", doorAt + new Vector3(x, 0, 0), new Vector3(0.08f, 2.1f, 0.06f), trimMat, false);
        Prim(decor, PrimitiveType.Sphere, "Door knob", doorAt + new Vector3(-0.36f, -0.05f, 0.05f), Vector3.one * 0.07f, metalMat);

        Material[] posters = { dieMat, wordsMat, sunsetMat };
        for (int i = 0; i < 3; i++)
        {
            Vector3 at = new Vector3(-1.5f + i * 1.1f, 1.95f, -halfD + 0.02f);
            Box(decor, "Poster frame", at, new Vector3(0.80f, 1.04f, 0.03f), plasticMat, false);
            GameObject poster = Prim(decor, PrimitiveType.Quad, "Poster", at + new Vector3(0, 0, 0.022f), new Vector3(0.74f, 0.98f, 1f), posters[i]);
            poster.transform.localEulerAngles = new Vector3(0, 180f, 0);   // faces the room
        }
        Vector3 boxesAt = new Vector3(-halfW + 0.5f, 0, -halfD + 0.5f);
        GameObject crate = Box(decor, "Box", boxesAt + new Vector3(0, 0.25f, 0), new Vector3(0.6f, 0.5f, 0.5f), cardboardMat);
        crate.transform.localEulerAngles = new Vector3(0, 12f, 0);
        GameObject crate2 = Box(decor, "Box", boxesAt + new Vector3(0.03f, 0.67f, 0.02f), new Vector3(0.45f, 0.34f, 0.4f), cardboardMat);
        crate2.transform.localEulerAngles = new Vector3(0, -9f, 0);
        GameObject crate3 = Box(decor, "Box", boxesAt + new Vector3(0.7f, 0.2f, -0.05f), new Vector3(0.5f, 0.4f, 0.42f), cardboardMat);
        crate3.transform.localEulerAngles = new Vector3(0, 31f, 0);

        // ---- a chair by the desk, out of the way of the keyboards
        Transform chair = new GameObject("Chair").transform;
        chair.SetParent(decor, false);
        chair.localPosition = new Vector3(-3.75f, 0, halfD - 1.55f);
        chair.localEulerAngles = new Vector3(0, 150f, 0);
        Prim(chair, PrimitiveType.Cylinder, "Chair base", new Vector3(0, 0.03f, 0), new Vector3(0.5f, 0.04f, 0.5f), metalMat);
        Prim(chair, PrimitiveType.Cylinder, "Chair post", new Vector3(0, 0.25f, 0), new Vector3(0.05f, 0.42f, 0.05f), metalMat);
        RBox(chair, "Chair seat", new Vector3(0, 0.48f, 0), new Vector3(0.46f, 0.08f, 0.46f), sofaMat, 0.035f);
        RBox(chair, "Chair back", new Vector3(0, 0.82f, -0.21f), new Vector3(0.44f, 0.5f, 0.07f), sofaMat, 0.03f);

        About(decor, halfW, plasticMat);

        // ---- light for whatever moves: probes through the room, and one reflection of it
        List<Vector3> probes = new List<Vector3>();
        for (float x = -halfW + 0.5f; x <= halfW - 0.4f; x += 1.7f)
            for (float z = -halfD + 0.5f; z <= halfD - 0.4f; z += 1.5f)
                foreach (float y in new[] { 0.25f, 1.4f, 2.7f })
                    probes.Add(new Vector3(x, y, z));
        LightProbeGroup group = new GameObject("Light probes").AddComponent<LightProbeGroup>();
        group.transform.SetParent(decor, false);
        group.probePositions = probes.ToArray();
        ReflectionProbe reflection = new GameObject("Reflection probe").AddComponent<ReflectionProbe>();
        reflection.transform.SetParent(decor, false);
        reflection.transform.localPosition = new Vector3(0, height / 2, 0);
        reflection.mode = ReflectionProbeMode.Baked;
        reflection.size = new Vector3(halfW * 2, height, halfD * 2);
        reflection.boxProjection = true;
        reflection.resolution = 128;

        // all of the light is baked: no sun, and only a little flat ambient under it
        RenderSettings.ambientMode = AmbientMode.Flat;
        RenderSettings.ambientLight = new Color(0.035f, 0.04f, 0.05f);
        RenderSettings.skybox = NightSky();
        foreach (GameObject go in world.gameObject.scene.GetRootGameObjects())
        {
            Light sun = go.GetComponent<Light>();
            if (sun != null) sun.gameObject.SetActive(false);
        }
    }

    // A board on the left wall for visitors: what this is, what to do with it, and whose work it
    // stands on. Text only, so nothing on it takes the laser.
    static void About(Transform decor, float halfW, Material frameMat)
    {
        const float wide = 2.3f, high = 1.7f, centreZ = -0.2f, centreY = 1.72f;
        Box(decor, "About frame", new Vector3(-halfW + 0.015f, centreY, centreZ), new Vector3(0.03f, high + 0.08f, wide + 0.08f), frameMat, false);
        RectTransform board = Panel(decor, "About", new Vector3(-halfW + 0.034f, centreY, centreZ), new Vector3(0, -90f, 0),
                                    wide * 1000, high * 1000, new Color(0.05f, 0.055f, 0.07f));
        Object.DestroyImmediate(board.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(board.GetComponent<BoxCollider>());
        Object.DestroyImmediate(board.GetComponent<UnityEngine.UI.GraphicRaycaster>());
        Color head = new Color(0.98f, 0.75f, 0.2f), body = new Color(0.86f, 0.88f, 0.92f), dim = new Color(0.6f, 0.63f, 0.7f);
        const float left = 60, columnWide = 1040, right = 1200, rightWide = 1040;

        Label(board, "Title", "A whole computer in a pixel shader", left, 40, 2180, 80, 60, TextAnchor.MiddleLeft, Color.white);

        Label(board, "What head", "WHAT IS RUNNING HERE", left, 150, columnWide, 44, 36, TextAnchor.MiddleLeft, head);
        Label(board, "What",
              "The screens on the front wall are a real computer: a 32-bit RISC-V processor, its memory " +
              "and its disk, all kept in textures. A shader on your graphics card runs tens of thousands of its " +
              "instructions every frame, a few million a second. It boots an ordinary Linux kernel and a small " +
              "desktop.\n\n" +
              "Nothing is streamed or simulated elsewhere. Every player's own graphics card runs a machine of " +
              "its own, so what you do here only you see.",
              left, 200, columnWide, 400, 33, TextAnchor.UpperLeft, body);

        Label(board, "Do head", "WHAT YOU CAN DO", left, 620, columnWide, 44, 36, TextAnchor.MiddleLeft, head);
        Label(board, "Do",
              "Press Power on the control panel. Linux boots in a few seconds and starts the desktop.\n\n" +
              "DISPLAY: each hand's beam is the mouse (trigger = left, grip = right, right stick = wheel).\n" +
              "Keyboards on the desk: both hands type, or press 'Use my keyboard' to use your real one.\n" +
              "Start menu (or the Win key): Terminal, Editor, Files, Web, Paint, Settings, Monitor, " +
              "Doom, glxgears and games. Web opens only a fixed list of sites, not the links inside " +
              "them: VRChat lets a world load only addresses it was built with.\n" +
              "CONSOLE: the machine's serial line, a Linux shell of its own.\n" +
              "MEMORY: all of the machine's memory at once; what is being written glows.\n" +
              "Speed: how many instructions the machine runs each frame.",
              left, 670, columnWide, 578, 33, TextAnchor.UpperLeft, body);

        Label(board, "How head", "WHY IT IS USABLE AT ALL", left, 1250, columnWide, 44, 36, TextAnchor.MiddleLeft, head);
        Label(board, "How",
              "The processor is slow and strictly one step at a time. So everything that touches many pixels or " +
              "bytes is handed to hardware of the machine's own, which is more shader passes: drawing, windows, " +
              "text, 3D, pictures, copies of memory. Doom's walls are drawn by the machine's GPU, not its CPU.",
              left, 1300, columnWide, 340, 33, TextAnchor.UpperLeft, body);

        Label(board, "Credits head", "BUILT ON THE WORK OF", right, 150, rightWide, 44, 36, TextAnchor.MiddleLeft, head);
        Label(board, "Credits",
              "rvc by pimaker: the RISC-V emulator in a shader this grew from, and its Linux port (linux-rvc, " +
              "kernel 5.17)\n" +
              "Linux, by Linus Torvalds and its many contributors\n" +
              "OpenSBI, in the boot image\n" +
              "musl libc, by Rich Felker and contributors\n" +
              "BusyBox: the shell and the command line tools\n" +
              "GCC, binutils, LLVM's clang and compiler-rt: the compilers and their runtime\n" +
              "Microwindows / Nano-X by Greg Haerr and contributors: the window system, its terminal, " +
              "calculator and games, and its port of Doom\n" +
              "DOOM by id Software: the source release, and the shareware episode's data\n" +
              "glxgears by Brian Paul, from the Mesa demos\n" +
              "Desktop photographs from Unsplash, and somebody's cats\n" +
              "Cascadia Mono by Microsoft: the console's font\n" +
              "VRChat's Worlds SDK, UdonSharp by Merlin, TextMesh Pro and Unity: this room",
              right, 200, rightWide, 1100, 33, TextAnchor.UpperLeft, body);

        Label(board, "Project head", "THE PROJECT", right, 1250, rightWide, 44, 36, TextAnchor.MiddleLeft, head);
        Label(board, "Project",
              "ShaderEmu by Michael Moroz. Sources, the desktop harness and documentation:\n" +
              "github.com/MichaelMoroz/ShaderEmu (MIT licence)",
              right, 1300, rightWide, 200, 33, TextAnchor.UpperLeft, body);
        Label(board, "Note", "Each of the projects above keeps its own licence.", right, 1560, rightWide, 60, 24, TextAnchor.MiddleLeft, dim);
    }

    [MenuItem("ShaderEmu/Bake lighting")]
    public static void BakeLighting()
    {
        LightingSettings settings = LoadOrCreate(Generated + "/Lighting.lighting", () => new LightingSettings());
        settings.bakedGI = true;
        settings.realtimeGI = false;
        settings.autoGenerate = false;
        // on the processor: the GPU lightmapper runs out of memory and stalls beside a headset
        settings.lightmapper = LightingSettings.Lightmapper.ProgressiveCPU;
        settings.mixedBakeMode = MixedLightingMode.Subtractive;
        settings.directionalityMode = LightmapsMode.NonDirectional;
        settings.lightmapResolution = 24f;   // texels a metre: fewer, and small objects share texels
        settings.lightmapMaxSize = 2048;
        settings.lightmapPadding = 4;
        settings.directSampleCount = 64;
        settings.indirectSampleCount = 512;
        settings.environmentSampleCount = 128;
        settings.maxBounces = 3;
        settings.ao = true;
        settings.aoMaxDistance = 0.7f;
        settings.filteringMode = LightingSettings.FilterMode.Auto;
        EditorUtility.SetDirty(settings);
        Lightmapping.lightingSettings = settings;
        foreach (Light light in RoomLights()) light.enabled = true;
        Lightmapping.bakeCompleted -= Baked;
        Lightmapping.bakeCompleted += Baked;
        Lightmapping.BakeAsync();
        Debug.Log("[ShaderEmu] lighting bake started");
    }

    static List<Light> RoomLights()
    {
        List<Light> lights = new List<Light>();
        foreach (GameObject root in UnityEngine.SceneManagement.SceneManager.GetActiveScene().GetRootGameObjects())
            foreach (Light light in root.GetComponentsInChildren<Light>(true))
                if (light.lightmapBakeType == LightmapBakeType.Baked) lights.Add(light);
        return lights;
    }

    // The lamps have done their work: off again, and the scene saved with its lightmap.
    static void Baked()
    {
        Lightmapping.bakeCompleted -= Baked;
        foreach (Light light in RoomLights()) light.enabled = false;
        UnityEngine.SceneManagement.Scene scene = UnityEngine.SceneManagement.SceneManager.GetActiveScene();
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(scene);
        UnityEditor.SceneManagement.EditorSceneManager.SaveScene(scene);
        Debug.Log("[ShaderEmu] lighting baked, lamps off, scene saved");
    }
}

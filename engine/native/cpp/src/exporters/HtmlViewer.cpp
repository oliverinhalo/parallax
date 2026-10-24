#include "parallax/exporters/HtmlViewer.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>
#include <sstream>

#include "parallax/io/ImageIo.h"

namespace parallax {
namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Escape for an HTML text node or a double-quoted attribute.
std::string escapeHtml(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

/// Reinterpret a float array as bytes. Both ends of this file are
/// little-endian in practice (every Android ABI and every desktop browser
/// host), and the JavaScript reads it back with a DataView that says so.
std::vector<uint8_t> toBytes(const std::vector<float>& values) {
  std::vector<uint8_t> bytes(values.size() * sizeof(float));
  if (!values.empty()) std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

std::vector<uint8_t> toBytes(const std::vector<uint32_t>& values) {
  std::vector<uint8_t> bytes(values.size() * sizeof(uint32_t));
  if (!values.empty()) std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

/**
 * The viewer itself.
 *
 * WebGL 1 rather than 2: it is the version every browser on every device the
 * recipient might open this on actually has, and nothing here needs more. The
 * shading is one directional light plus a hemisphere term, which is enough to
 * read the shape of a scan and short enough to inline.
 */
const char* kViewerScript = R"JS(
(function () {
  var data = window.PARALLAX_MODEL;
  var canvas = document.getElementById('scene');
  var gl = canvas.getContext('webgl', { antialias: true, alpha: false });
  var fallback = document.getElementById('fallback');
  if (!gl) { fallback.style.display = 'block'; return; }

  function bytes(base64) {
    var binary = atob(base64);
    var out = new Uint8Array(binary.length);
    for (var i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
    return out;
  }
  function floats(base64) {
    var raw = bytes(base64);
    return new Float32Array(raw.buffer, raw.byteOffset, raw.byteLength / 4);
  }
  function uints(base64) {
    var raw = bytes(base64);
    return new Uint32Array(raw.buffer, raw.byteOffset, raw.byteLength / 4);
  }

  var positions = floats(data.positions);
  var normals = data.normals ? floats(data.normals) : null;
  var uvs = data.uvs ? floats(data.uvs) : null;
  var colors = data.colors ? bytes(data.colors) : null;
  var indices = uints(data.indices);

  var vertexSource = [
    'attribute vec3 aPosition;',
    'attribute vec3 aNormal;',
    'attribute vec2 aUv;',
    'attribute vec3 aColor;',
    'uniform mat4 uViewProjection;',
    'varying vec3 vNormal;',
    'varying vec2 vUv;',
    'varying vec3 vColor;',
    'void main() {',
    '  vNormal = aNormal;',
    '  vUv = aUv;',
    '  vColor = aColor;',
    '  gl_Position = uViewProjection * vec4(aPosition, 1.0);',
    '}'
  ].join('\n');

  var fragmentSource = [
    'precision mediump float;',
    'varying vec3 vNormal;',
    'varying vec2 vUv;',
    'varying vec3 vColor;',
    'uniform sampler2D uTexture;',
    'uniform int uHasTexture;',
    'uniform int uHasColor;',
    'uniform int uShowNormals;',
    'void main() {',
    '  vec3 base = vec3(0.78);',
    '  if (uHasTexture == 1) base = texture2D(uTexture, vUv).rgb;',
    '  else if (uHasColor == 1) base = vColor;',
    '  vec3 n = normalize(vNormal);',
    '  if (uShowNormals == 1) { gl_FragColor = vec4(n * 0.5 + 0.5, 1.0); return; }',
    '  float key = max(dot(n, normalize(vec3(0.4, 0.8, 0.45))), 0.0);',
    '  float sky = 0.5 + 0.5 * n.y;',
    '  vec3 lit = base * (0.25 + 0.65 * key + 0.25 * sky);',
    '  gl_FragColor = vec4(pow(lit, vec3(1.0 / 2.2)), 1.0);',
    '}'
  ].join('\n');

  function compile(type, source) {
    var shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      throw new Error(gl.getShaderInfoLog(shader));
    }
    return shader;
  }
  var program = gl.createProgram();
  gl.attachShader(program, compile(gl.VERTEX_SHADER, vertexSource));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fragmentSource));
  gl.linkProgram(program);
  gl.useProgram(program);

  function buffer(target, source) {
    var handle = gl.createBuffer();
    gl.bindBuffer(target, handle);
    gl.bufferData(target, source, gl.STATIC_DRAW);
    return handle;
  }
  var positionBuffer = buffer(gl.ARRAY_BUFFER, positions);
  var normalBuffer = normals ? buffer(gl.ARRAY_BUFFER, normals) : null;
  var uvBuffer = uvs ? buffer(gl.ARRAY_BUFFER, uvs) : null;
  var colorBuffer = colors ? buffer(gl.ARRAY_BUFFER, colors) : null;

  // WebGL 1 indexes with 16-bit integers unless OES_element_index_uint is
  // available. A scan always has more than 65,536 vertices, so without the
  // extension the model is drawn in 16-bit chunks instead of not at all.
  var uintIndices = gl.getExtension('OES_element_index_uint');
  var indexBuffer;
  var indexType;
  var drawCount = indices.length;
  var chunks = [];
  if (uintIndices) {
    indexBuffer = buffer(gl.ELEMENT_ARRAY_BUFFER, indices);
    indexType = gl.UNSIGNED_INT;
  } else {
    indexType = gl.UNSIGNED_SHORT;
    var chunkSize = 65535;
    for (var start = 0; start < positions.length / 3; start += chunkSize) {
      var end = Math.min(start + chunkSize, positions.length / 3);
      var local = [];
      for (var t = 0; t < indices.length; t += 3) {
        var a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= start && a < end && b >= start && b < end && c >= start && c < end) {
          local.push(a - start, b - start, c - start);
        }
      }
      if (!local.length) continue;
      chunks.push({
        offset: start,
        buffer: buffer(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(local)),
        count: local.length
      });
    }
  }

  var texture = null;
  if (data.texture) {
    texture = gl.createTexture();
    var image = new Image();
    image.onload = function () {
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB, gl.RGB, gl.UNSIGNED_BYTE, image);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      ready = true;
    };
    image.src = 'data:image/png;base64,' + data.texture;
  }
  var ready = !data.texture;

  var centre = data.center;
  var radius = Math.max(data.radius, 1e-3);
  var state = { yaw: 0.6, pitch: 0.35, distance: radius * 2.6, panX: 0, panY: 0 };

  function multiply(a, b) {
    var out = new Float32Array(16);
    for (var r = 0; r < 4; r++) {
      for (var c = 0; c < 4; c++) {
        var sum = 0;
        for (var k = 0; k < 4; k++) sum += a[r * 4 + k] * b[k * 4 + c];
        out[r * 4 + c] = sum;
      }
    }
    return out;
  }

  function viewProjection() {
    var aspect = canvas.width / Math.max(canvas.height, 1);
    var fov = 45 * Math.PI / 180;
    var near = radius * 0.01, far = radius * 40;
    var f = 1 / Math.tan(fov / 2);
    var projection = new Float32Array([
      f / aspect, 0, 0, 0,
      0, f, 0, 0,
      0, 0, (far + near) / (near - far), 2 * far * near / (near - far),
      0, 0, -1, 0
    ]);

    var ex = centre[0] + state.distance * Math.cos(state.pitch) * Math.sin(state.yaw);
    var ey = centre[1] + state.distance * Math.sin(state.pitch);
    var ez = centre[2] + state.distance * Math.cos(state.pitch) * Math.cos(state.yaw);
    var tx = centre[0] + state.panX, ty = centre[1] + state.panY, tz = centre[2];

    var fx = tx - ex, fy = ty - ey, fz = tz - ez;
    var fl = Math.hypot(fx, fy, fz) || 1; fx /= fl; fy /= fl; fz /= fl;
    var rx = fy * 0 - fz * 1, ry = fz * 0 - fx * 0, rz = fx * 1 - fy * 0;
    var rl = Math.hypot(rx, ry, rz) || 1; rx /= rl; ry /= rl; rz /= rl;
    var ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
    var view = new Float32Array([
      rx, ry, rz, -(rx * ex + ry * ey + rz * ez),
      ux, uy, uz, -(ux * ex + uy * ey + uz * ez),
      -fx, -fy, -fz, (fx * ex + fy * ey + fz * ez),
      0, 0, 0, 1
    ]);
    return multiply(projection, view);
  }

  function attribute(name, handle, size, type, normalized) {
    var location = gl.getAttribLocation(program, name);
    if (location < 0) return;
    if (!handle) { gl.disableVertexAttribArray(location); return; }
    gl.enableVertexAttribArray(location);
    gl.bindBuffer(gl.ARRAY_BUFFER, handle);
    gl.vertexAttribPointer(location, size, type, normalized, 0, 0);
  }

  var showNormals = false;
  function draw() {
    var width = canvas.clientWidth, height = canvas.clientHeight;
    var ratio = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.floor(width * ratio);
    canvas.height = Math.floor(height * ratio);
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clearColor(data.background[0], data.background[1], data.background[2], 1);
    gl.enable(gl.DEPTH_TEST);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (!ready) { requestAnimationFrame(draw); return; }

    gl.useProgram(program);
    gl.uniformMatrix4fv(gl.getUniformLocation(program, 'uViewProjection'), false, viewProjection());
    gl.uniform1i(gl.getUniformLocation(program, 'uHasTexture'), texture ? 1 : 0);
    gl.uniform1i(gl.getUniformLocation(program, 'uHasColor'), colors ? 1 : 0);
    gl.uniform1i(gl.getUniformLocation(program, 'uShowNormals'), showNormals ? 1 : 0);
    if (texture) {
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.uniform1i(gl.getUniformLocation(program, 'uTexture'), 0);
    }
    attribute('aPosition', positionBuffer, 3, gl.FLOAT, false);
    attribute('aNormal', normalBuffer, 3, gl.FLOAT, false);
    attribute('aUv', uvBuffer, 2, gl.FLOAT, false);
    attribute('aColor', colorBuffer, 3, gl.UNSIGNED_BYTE, true);

    if (uintIndices) {
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
      gl.drawElements(gl.TRIANGLES, drawCount, indexType, 0);
    } else {
      for (var i = 0; i < chunks.length; i++) {
        var chunk = chunks[i];
        attribute('aPosition', positionBuffer, 3, gl.FLOAT, false);
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, chunk.buffer);
        gl.drawElements(gl.TRIANGLES, chunk.count, indexType, 0);
      }
    }
    requestAnimationFrame(draw);
  }

  var dragging = null;
  canvas.addEventListener('pointerdown', function (event) {
    dragging = { x: event.clientX, y: event.clientY, shift: event.shiftKey };
    canvas.setPointerCapture(event.pointerId);
  });
  canvas.addEventListener('pointerup', function () { dragging = null; });
  canvas.addEventListener('pointermove', function (event) {
    if (!dragging) return;
    var dx = (event.clientX - dragging.x) / canvas.clientWidth;
    var dy = (event.clientY - dragging.y) / canvas.clientHeight;
    dragging.x = event.clientX;
    dragging.y = event.clientY;
    if (dragging.shift) {
      state.panX -= dx * state.distance;
      state.panY += dy * state.distance;
    } else {
      state.yaw -= dx * 3.2;
      state.pitch = Math.max(-1.55, Math.min(1.55, state.pitch + dy * 3.2));
    }
  });
  canvas.addEventListener('wheel', function (event) {
    event.preventDefault();
    state.distance = Math.max(radius * 0.1,
      Math.min(radius * 20, state.distance * (1 + event.deltaY * 0.0015)));
  }, { passive: false });

  var pinch = null;
  canvas.addEventListener('touchmove', function (event) {
    if (event.touches.length !== 2) { pinch = null; return; }
    event.preventDefault();
    var spread = Math.hypot(
      event.touches[0].clientX - event.touches[1].clientX,
      event.touches[0].clientY - event.touches[1].clientY);
    if (pinch) {
      state.distance = Math.max(radius * 0.1,
        Math.min(radius * 20, state.distance * pinch / spread));
    }
    pinch = spread;
  }, { passive: false });
  canvas.addEventListener('touchend', function () { pinch = null; });

  document.getElementById('normals').addEventListener('click', function (event) {
    showNormals = !showNormals;
    event.target.textContent = showNormals ? 'Shaded' : 'Normals';
  });
  document.getElementById('reset').addEventListener('click', function () {
    state = { yaw: 0.6, pitch: 0.35, distance: radius * 2.6, panX: 0, panY: 0 };
  });

  requestAnimationFrame(draw);
})();
)JS";

const char* kStyle = R"CSS(
:root { color-scheme: dark; }
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; }
body {
  background: #0b0c0e; color: #eef1f5; display: flex; flex-direction: column;
  font: 15px/1.5 ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
header { padding: 16px 20px 10px; }
h1 { margin: 0 0 4px; font-size: 19px; font-weight: 650; letter-spacing: -0.01em; }
.facts { display: flex; flex-wrap: wrap; gap: 6px 14px; color: #9aa3ae; font-size: 13px; }
.facts b { color: #eef1f5; font-weight: 550; font-variant-numeric: tabular-nums; }
main { position: relative; flex: 1 1 auto; min-height: 0; }
canvas { display: block; width: 100%; height: 100%; touch-action: none; }
.controls { position: absolute; left: 16px; bottom: 16px; display: flex; gap: 8px; }
button {
  appearance: none; border: 1px solid rgba(255,255,255,0.14); border-radius: 999px;
  background: rgba(20,22,26,0.72); color: #eef1f5; padding: 9px 16px;
  font: inherit; font-size: 13px; cursor: pointer; backdrop-filter: blur(12px);
}
button:hover { border-color: rgba(61,224,255,0.5); }
button:focus-visible { outline: 2px solid #3de0ff; outline-offset: 2px; }
.hint { position: absolute; right: 16px; bottom: 18px; color: #6f7883; font-size: 12px; }
#fallback { display: none; padding: 20px; color: #9aa3ae; }
footer { padding: 10px 20px 16px; color: #6f7883; font-size: 12px; }
)CSS";

}  // namespace

std::string base64Encode(const uint8_t* data, size_t length) {
  std::string out;
  out.reserve((length + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < length) {
    const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                            (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out += kBase64Alphabet[(triple >> 18) & 0x3F];
    out += kBase64Alphabet[(triple >> 12) & 0x3F];
    out += kBase64Alphabet[(triple >> 6) & 0x3F];
    out += kBase64Alphabet[triple & 0x3F];
    i += 3;
  }
  if (i < length) {
    const uint32_t remaining = length - i;
    uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
    if (remaining == 2) triple |= static_cast<uint32_t>(data[i + 1]) << 8;
    out += kBase64Alphabet[(triple >> 18) & 0x3F];
    out += kBase64Alphabet[(triple >> 12) & 0x3F];
    out += remaining == 2 ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=';
    out += '=';
  }
  return out;
}

std::string buildHtmlViewer(const Mesh& mesh, const ColorBuffer& texture,
                            const HtmlViewerOptions& options) {
  Mesh working = mesh;
  if (options.target_triangles > 0 &&
      working.triangleCount() > static_cast<size_t>(options.target_triangles)) {
    decimateMesh(&working, options.target_triangles);
  }
  if (!working.hasNormals()) recomputeNormals(&working);

  std::vector<float> positions(working.vertexCount() * 3);
  for (size_t v = 0; v < working.vertexCount(); ++v) {
    positions[v * 3 + 0] = working.vertices[v].x();
    positions[v * 3 + 1] = working.vertices[v].y();
    positions[v * 3 + 2] = working.vertices[v].z();
  }
  std::vector<float> normals;
  if (working.hasNormals()) {
    normals.resize(working.vertexCount() * 3);
    for (size_t v = 0; v < working.vertexCount(); ++v) {
      normals[v * 3 + 0] = working.normals[v].x();
      normals[v * 3 + 1] = working.normals[v].y();
      normals[v * 3 + 2] = working.normals[v].z();
    }
  }
  std::vector<float> uvs;
  if (working.hasUvs()) {
    uvs.resize(working.vertexCount() * 2);
    for (size_t v = 0; v < working.vertexCount(); ++v) {
      uvs[v * 2 + 0] = working.uvs[v].x();
      uvs[v * 2 + 1] = working.uvs[v].y();
    }
  }
  std::vector<uint8_t> colors;
  if (working.hasColors()) {
    colors.resize(working.vertexCount() * 3);
    for (size_t v = 0; v < working.vertexCount(); ++v) {
      colors[v * 3 + 0] = working.colors[v][0];
      colors[v * 3 + 1] = working.colors[v][1];
      colors[v * 3 + 2] = working.colors[v][2];
    }
  }

  const AxisAlignedBox bounds = meshBounds(working);
  const Vec3 centre = bounds.valid() ? bounds.center() : Vec3::Zero();
  const double radius = bounds.valid() ? 0.5 * bounds.diagonal() : 1.0;

  std::string texture_base64;
  if (!texture.empty() && working.hasUvs()) {
    ColorBuffer scaled = downscaleToWidth(texture, options.max_texture_size);
    std::vector<uint8_t> png;
    if (encodePng(scaled, &png)) texture_base64 = base64Encode(png);
  }

  std::ostringstream model;
  model.precision(9);
  model << "window.PARALLAX_MODEL={";
  model << "\"positions\":\"" << base64Encode(toBytes(positions)) << "\"";
  if (!normals.empty()) model << ",\"normals\":\"" << base64Encode(toBytes(normals)) << "\"";
  if (!uvs.empty()) model << ",\"uvs\":\"" << base64Encode(toBytes(uvs)) << "\"";
  if (!colors.empty()) model << ",\"colors\":\"" << base64Encode(colors) << "\"";
  model << ",\"indices\":\"" << base64Encode(toBytes(working.indices)) << "\"";
  if (!texture_base64.empty()) model << ",\"texture\":\"" << texture_base64 << "\"";
  model << ",\"center\":[" << centre.x() << "," << centre.y() << "," << centre.z() << "]";
  model << ",\"radius\":" << radius;
  model << ",\"background\":"
        << (options.dark ? "[0.043,0.047,0.055]" : "[0.93,0.94,0.95]");
  model << ",\"triangles\":" << working.triangleCount();
  model << "};";

  std::ostringstream out;
  out << "<!doctype html>\n<html lang=\"en\">\n<head>\n";
  out << "<meta charset=\"utf-8\">\n";
  out << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
  out << "<title>" << escapeHtml(options.title) << "</title>\n";
  out << "<style>" << kStyle << "</style>\n</head>\n<body>\n";
  out << "<header>\n<h1>" << escapeHtml(options.title) << "</h1>\n<div class=\"facts\">\n";
  for (const auto& fact : options.facts) {
    out << "<span>" << escapeHtml(fact.first) << " <b>" << escapeHtml(fact.second)
        << "</b></span>\n";
  }
  out << "</div>\n</header>\n";
  out << "<main>\n<canvas id=\"scene\" aria-label=\"" << escapeHtml(options.title)
      << ", an interactive 3D model\" role=\"img\"></canvas>\n";
  out << "<div id=\"fallback\">This browser does not support WebGL, so the model cannot be "
         "shown here.</div>\n";
  out << "<div class=\"controls\"><button id=\"reset\" type=\"button\">Reset view</button>"
         "<button id=\"normals\" type=\"button\">Normals</button></div>\n";
  out << "<p class=\"hint\">Drag to orbit &middot; shift-drag to pan &middot; scroll to "
         "zoom</p>\n";
  out << "</main>\n";
  out << "<footer>Captured with Parallax. This file is self-contained and works offline."
         "</footer>\n";
  out << "<script>" << model.str() << "</script>\n";
  out << "<script>" << kViewerScript << "</script>\n";
  out << "</body>\n</html>\n";
  return out.str();
}

bool writeHtmlViewer(const std::string& path, const Mesh& mesh, const ColorBuffer& texture,
                     const HtmlViewerOptions& options) {
  if (mesh.empty()) return false;
  const std::string document = buildHtmlViewer(mesh, texture, options);
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  file.write(document.data(), static_cast<std::streamsize>(document.size()));
  return file.good();
}

}  // namespace parallax

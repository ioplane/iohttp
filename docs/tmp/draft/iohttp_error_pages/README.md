# iohttp Error Pages

Beautiful, responsive error pages for iohttp HTTP server.

## Features

- **Modern Design** — Clean, gradient-based design with glassmorphism effects
- **Responsive** — Works perfectly on all devices
- **Bootstrap 5** — Built with Bootstrap 5.3.2 and Bootstrap Icons
- **Animations** — Smooth entrance animations and hover effects
- **Branding** — Includes iohttp branding in footer

## Error Codes Included

| Code | Name | Icon | Color Theme |
|------|------|------|-------------|
| 400 | Bad Request | ⚠️ exclamation-triangle | Red gradient |
| 401 | Unauthorized | 🔒 shield-lock | Pink gradient |
| 403 | Forbidden | 🚫 ban | Orange-red gradient |
| 404 | Not Found | ❓ question-circle | Purple gradient |
| 405 | Method Not Allowed | ❌ x-octagon | Pink gradient |
| 408 | Request Timeout | ⏰ clock-history | Orange gradient |
| 429 | Too Many Requests | 🏎️ speedometer | Red gradient |
| 500 | Internal Server Error | 🐛 bug | Red gradient |
| 502 | Bad Gateway | 🌐 hdd-network | Purple gradient |
| 503 | Service Unavailable | 🔧 tools | Orange gradient |
| 504 | Gateway Timeout | 📡 wifi-off | Red gradient |

## Usage

### Static Files

Configure iohttp to serve these files as static error pages:

```c
io_server_config_t cfg = {
    .error_pages = {
        .path_400 = "/var/www/errors/400.html",
        .path_401 = "/var/www/errors/401.html",
        .path_403 = "/var/www/errors/403.html",
        .path_404 = "/var/www/errors/404.html",
        .path_405 = "/var/www/errors/405.html",
        .path_408 = "/var/www/errors/408.html",
        .path_429 = "/var/www/errors/429.html",
        .path_500 = "/var/www/errors/500.html",
        .path_502 = "/var/www/errors/502.html",
        .path_503 = "/var/www/errors/503.html",
        .path_504 = "/var/www/errors/504.html",
    }
};
```

### Embedded (C23 #embed)

Alternatively, embed directly in your C code:

```c
static const char error_404[] = {
#embed "/path/to/404.html"
};
```

## Customization

### Change Colors

Edit the CSS variables in each file:

```css
:root {
    --primary-gradient: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    --error-color: #9b59b6;
}
```

### Change Branding

Replace "iohttp" in the footer:

```html
<div class="server-info">
    <i class="bi bi-server"></i> Powered by <strong>Your Server</strong>
</div>
```

### Add Custom Buttons

Add buttons to the action area:

```html
<a href="/contact" class="btn-home">
    <i class="bi bi-envelope-fill"></i>
    Contact Support
</a>
```

## CDN Dependencies

- Bootstrap 5.3.2 CSS: `https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css`
- Bootstrap Icons 1.11.1: `https://cdn.jsdelivr.net/npm/bootstrap-icons@1.11.1/font/bootstrap-icons.css`

For offline use, download these files and update the links.

## License

MIT License — Free to use and modify.

## Credits

Created for iohttp — High-performance HTTP server with io_uring and wolfSSL.

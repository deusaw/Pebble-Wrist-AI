#
# This file is the default set of rules to compile a Pebble application.
#
# Feel free to customize this to your needs.
#
import os.path

top = '.'
out = 'build'


def bundle_legacy_pkjs(task):
    """Vendor OpenCC into the traditional single-file iOS PKJS payload."""
    opencc_path = task.inputs[0].abspath()
    app_path = task.inputs[1].abspath()
    target_path = task.outputs[0].abspath()
    with open(opencc_path, 'r', encoding='utf-8') as source:
        opencc = source.read()
    with open(app_path, 'r', encoding='utf-8') as source:
        app = source.read()
    require_line = "var OpenCC = require('opencc-js/t2cn');"
    if require_line not in app:
        raise ValueError('OpenCC require marker not found in PKJS entry')
    # Force the UMD package down its browser-global path even if the companion
    # loader exposes CommonJS-like module/exports shims.
    vendored_opencc = (
        "(function() { var module, exports, define;\n" +
        opencc +
        "\n}).call(this);\n"
        "var OpenCC = (typeof globalThis !== 'undefined' && globalThis.OpenCC) "
        "? globalThis.OpenCC : this.OpenCC;\n"
    )
    app = app.replace(require_line, vendored_opencc, 1)
    with open(target_path, 'w', encoding='utf-8', newline='') as target:
        target.write(app)


def options(ctx):
    ctx.load('pebble_sdk')


def configure(ctx):
    ctx.load('pebble_sdk')


def build(ctx):
    ctx.load('pebble_sdk')

    build_worker = os.path.exists('worker_src')
    binaries = []

    cached_env = ctx.env
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        ctx.set_group(ctx.env.PLATFORM_NAME)
        app_elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_build(source=ctx.path.ant_glob('src/c/**/*.c'), target=app_elf, bin_type='app')

        if build_worker:
            worker_elf = '{}/pebble-worker.elf'.format(ctx.env.BUILD_DIR)
            binaries.append({'platform': platform, 'app_elf': app_elf, 'worker_elf': worker_elf})
            ctx.pbl_build(source=ctx.path.ant_glob('worker_src/c/**/*.c'),
                          target=worker_elf,
                          bin_type='worker')
        else:
            binaries.append({'platform': platform, 'app_elf': app_elf})
    ctx.env = cached_env

    ctx.set_group('bundle')
    pkjs_source = ctx.path.find_node('src/pkjs/pebble-js-app.js')
    opencc_source = ctx.path.find_node('node_modules/opencc-js/dist/umd/t2cn.js')
    if not pkjs_source or not opencc_source:
        ctx.fatal('PKJS source or OpenCC t2cn bundle is missing; run npm install')
    bundled_pkjs = ctx.path.get_bld().make_node('pkjs/pebble-js-app.js')
    bundled_pkjs.parent.mkdir()
    ctx(rule=bundle_legacy_pkjs,
        source=[opencc_source, pkjs_source],
        target=bundled_pkjs,
        name='bundle_legacy_pkjs')
    ctx.pbl_bundle(binaries=binaries,
                   js=[bundled_pkjs],
                   js_entry_file='build/pkjs/pebble-js-app.js')

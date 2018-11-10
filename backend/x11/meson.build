x11_libs = []
x11_required = [
	'xcb',
	'xcb-dri3',
	'xcb-present',
	'xcb-xfixes',
	'xcb-xinput',
]
x11_optional = [
	'xcb-xkb',
]

foreach lib : x11_required
	dep = dependency(lib, required: get_option('x11-backend'))
	if not dep.found()
		subdir_done()
	endif

	x11_libs += dep
endforeach

foreach lib : x11_optional
	dep = dependency(lib, required: get_option(lib))
	if dep.found()
		x11_libs += dep
		conf_data.set('WLR_HAS_' + lib.underscorify().to_upper(), true)
	endif
endforeach

backend_files += files(
	'backend.c',
	'input_device.c',
	'output.c',
)
backend_deps += x11_libs
conf_data.set('WLR_HAS_X11_BACKEND', true)

cmake_minimum_required(VERSION 3.15)

project(wsl-mount-luks LANGUAGES C)

if(WIN32)
	add_compile_definitions(UNICODE _UNICODE)

	add_executable(wsl-mount-findfs wsl-mount-findfs.c IntegrityLevel.c utf8.manifest)
	target_link_options(wsl-mount-findfs PRIVATE /MANIFESTUAC:level='requireAdministrator')
	target_link_libraries(wsl-mount-findfs PRIVATE setupapi)

	add_executable(luks-askpass-wincred luks-askpass-wincred.c utf8.manifest)
	target_link_libraries(luks-askpass-wincred PRIVATE Credui)
else()
	# I think this will probably also have linux-side utilities
endif()
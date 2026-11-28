"""Functions used to generate source files during build time"""

import os

import methods


def make_models_header(target, source, env):
    # The MediaPipe models, built into the engine so a game needs no files of
    # its own for eye tracking.
    with methods.generated_wrapper(str(target[0])) as file:
        for src in map(str, source):
            buffer = methods.get_buffer(src)
            name = os.path.splitext(os.path.basename(src))[0]
            file.write(f"""inline constexpr int _eye_model_{name}_size = {len(buffer)};
inline constexpr unsigned char _eye_model_{name}[] = {{
{methods.format_buffer(buffer, 1)}
}};

""")

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <SDL.h>
#include <glad/glad.h>

static void fatal(const char* msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bee Sim Error", msg, NULL);
}

/* ---- shader helpers that DON'T exit on error ---- */
static GLuint compile(GLenum type, const char* src, int* ok_out, char* log_out, int log_cap) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, log_cap, NULL, log_out);
        *ok_out = 0;
    } else {
        *ok_out = 1;
        if (log_out) log_out[0] = 0;
    }
    return s;
}
static GLuint link_program(GLuint vs, GLuint fs, int* ok_out, char* log_out, int log_cap) {
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) glGetProgramInfoLog(p, log_cap, NULL, log_out);
    *ok_out = ok;
    return p;
}

/* 2D circle-in-quad shaders (GL 3.3 core) */
static const char* VS = "#version 330 core\n"
"layout(location=0) in vec2 a_pos;\n"
"uniform vec2 u_screen, u_pos, u_size;\n"
"out vec2 v_px;\n"
"void main(){ vec2 px = u_pos + a_pos * u_size; v_px = px;\n"
" vec2 ndc = vec2((px.x/u_screen.x)*2.0-1.0, 1.0-(px.y/u_screen.y)*2.0);\n"
" gl_Position = vec4(ndc,0,1); }\n";

static const char* FS = "#version 330 core\n"
"in vec2 v_px; out vec4 frag;\n"
"uniform vec2 u_center; uniform float u_radius; uniform vec4 u_color;\n"
"void main(){ float d = distance(v_px,u_center);\n"
" float a = smoothstep(u_radius, u_radius-1.5, d);\n"
" frag = vec4(u_color.rgb, u_color.a * a); }\n";

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fatal(SDL_GetError()); return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* win = SDL_CreateWindow("Bee Circle",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 900, 900,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { fatal(SDL_GetError()); return 1; }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fatal("Failed to load OpenGL via glad"); return 1;
    }

    /* Print GL info so we know if you're on GDI 1.1 or a real driver */
    const char* ver = (const char*)glGetString(GL_VERSION);
    const char* ven = (const char*)glGetString(GL_VENDOR);
    const char* ren = (const char*)glGetString(GL_RENDERER);
    printf("OpenGL: %s\nVendor : %s\nRenderer: %s\n", ver?ver:"(null)", ven?ven:"(null)", ren?ren:"(null)");

    /* If you see OpenGL 1.1 / Microsoft / GDI Generic, you need a GPU driver or to avoid Remote Desktop. */

    /* Build pipeline but don't exit on errors */
    int ok_vs=0, ok_fs=0, ok_link=0;
    char log[2048];
    GLuint vs = compile(GL_VERTEX_SHADER, VS, &ok_vs, log, sizeof log);
    if (!ok_vs) { fprintf(stderr, "Vertex shader error:\n%s\n", log); }
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS, &ok_fs, log, sizeof log);
    if (!ok_fs) { fprintf(stderr, "Fragment shader error:\n%s\n", log); }
    GLuint prog = 0;
    if (ok_vs && ok_fs) {
        prog = link_program(vs, fs, &ok_link, log, sizeof log);
        if (!ok_link) fprintf(stderr, "Program link error:\n%s\n", log);
    }
    glDeleteShader(vs); glDeleteShader(fs);

    /* Geometry */
    float quad[] = { 0,0,  1,0,  0,1,  1,1 };
    GLuint vao=0, vbo=0;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,2*sizeof(float),(void*)0);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Uniform locations (only if program linked) */
    GLint u_screen=-1,u_pos=-1,u_size=-1,u_center=-1,u_radius=-1,u_color=-1;
    if (ok_link) {
        u_screen = glGetUniformLocation(prog,"u_screen");
        u_pos    = glGetUniformLocation(prog,"u_pos");
        u_size   = glGetUniformLocation(prog,"u_size");
        u_center = glGetUniformLocation(prog,"u_center");
        u_radius = glGetUniformLocation(prog,"u_radius");
        u_color  = glGetUniformLocation(prog,"u_color");
    }

    float cx=450, cy=450, radius=40, t=0;

    int running=1; SDL_Event e;
    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type==SDL_QUIT) running=0;
            if (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) running=0;
        }
        int w,h; SDL_GetWindowSize(win,&w,&h);
        glViewport(0,0,w,h);
        glClearColor(1,1,1,1); glClear(GL_COLOR_BUFFER_BIT);

        if (ok_link) {
            t += 0.016f; float ybob = sinf(t*2.f)*10.f;
            glUseProgram(prog);
            glUniform2f(u_screen,(float)w,(float)h);
            glUniform2f(u_pos, cx-radius, cy-radius+ybob);
            glUniform2f(u_size, radius*2.f, radius*2.f);
            glUniform2f(u_center, cx, cy+ybob);
            glUniform1f(u_radius, radius);
            glUniform4f(u_color, 0.1f,0.1f,0.1f,1.0f);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        } else {
            /* Shaders failed—draw nothing, keep window up so you can read logs */
        }

        SDL_GL_SwapWindow(win);
    }

    if (prog) glDeleteProgram(prog);
    glDeleteBuffers(1,&vbo);
    glDeleteVertexArrays(1,&vao);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}

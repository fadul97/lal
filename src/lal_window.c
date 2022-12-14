#include "lal_defines.h"
#include "lal_error_list.h"
#include "lal/lal_window.h"
#include "lal/lal_input.h"

#if LPLATFORM_LINUX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <X11/Xlib-xcb.h>
#include <X11/XKBlib.h>

#define GLX_GLXEXT_PROTOTYPES
#include <GL/glx.h>
#include <GL/glxext.h>

typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

typedef struct WindowX11
{
	Display *display;
	ulong32 id;
	Screen screen;
	ulong32 delete_msg;
} WindowX11;

typedef struct WindowX11GL
{
	Display *display;
	ulong32 id;
	Screen *screen;
    sint32 screen_id;
	ulong32 delete_msg;
    GLXContext context;
    XWindowAttributes window_attribs;
} WindowX11GL;

typedef struct WindowXCBGL
{
	Display *display;
    ulong32 x11_id;
    sint32 screen_id;
	uint32 xcb_id;
    xcb_connection_t *xcb_connection;
    xcb_screen_t *xcb_screen;
    uint32 delete_msg;
	uint32 wm_protocols;
    XWindowAttributes window_attribs;
    GLXContext context;
    ulong32 glx_id;
    uint32 xcb_colormap;
    GLXFBConfig glx_fb_config;
} WindowXCBGL;

Keys translate_keycode(uint32 key);
b8 isExtensionSupported(const char *extList, const char *extension);

b8 create_simple_window(
		PlatformHandler *platform_handler,
		uint32 x,
		uint32 y,
		uint32 width,
		uint32 height)
{
	// Create WindowX11
	platform_handler->window = malloc(sizeof(WindowX11));
	WindowX11 *window = (WindowX11 *)platform_handler->window;
	
	// Open a connection to X server
	window->display = XOpenDisplay(NULL);
	if(window->display == NULL)
	{
		printf("ERROR: Failed to open display.\n");
		return WINDOW_ERROR;
	}

	// Setup window config
	window->id = XCreateSimpleWindow(
			window->display,					// display
			DefaultRootWindow(window->display),	// parent
			x,									// x pos
			y,									// y pos
			width,								// width
			height,								// height
			0,									// border width
			0,									// border
			0);									// background

	// Report events associated with specified event mask
	XSelectInput(window->display, window->id, KeyPressMask | KeyReleaseMask);

	// Map window by client application
	XMapWindow(window->display, window->id);
	
	// Flush output buffer and wait for requests processed by X server
	XSync(window->display, 0);
	
	// Variable to hold the delete window message
	window->delete_msg = XInternAtom(window->display, "WM_DELETE_WINDOW", 0);
	XSetWMProtocols(window->display, window->id, &window->delete_msg, 1);

	// Disable key repeat
	XAutoRepeatOff(window->display);

	// Initialize Input system
	input_initialize();

	// Set running to false
	platform_handler->running = TRUE;

	return OK;
}

b8 create_gl_xlib_window(
		PlatformHandler *platform_handler,
		const char* window_title,
		uint32 x,
		uint32 y,
		uint32 width,
		uint32 height)
{
    platform_handler->window = malloc(sizeof(WindowX11GL));
    WindowX11GL *window = (WindowX11GL *)platform_handler->window;

    // Open Display
    window->display = XOpenDisplay(NULL);
    if(window->display == NULL)
    {
        printf("ERROR: Failed to open display.\n");
        return WINDOW_ERROR;
    }

    // Initialize Screen
    window->screen = DefaultScreenOfDisplay(window->display);
    window->screen_id = DefaultScreen(window->display);
    
    printf("Finding GL versions...\n");
    sint32 major_version = 0;
    sint32 minor_version = 0;

    // Get GL versions
    glXQueryVersion(window->display, &major_version, &minor_version);
    if(major_version <= 1 && minor_version < 2)
    {
        printf("ERROR: GLX 1.2 or greater is required.\n");
        XCloseDisplay(window->display);
        return CONTEXT_ERROR;
    }

    printf("GLX version: %d.%d\n", major_version, minor_version);

    sint32 glx_attribs[] = {
        GLX_X_RENDERABLE,   True,
		GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT,
		GLX_RENDER_TYPE,    GLX_RGBA_BIT,
		GLX_X_VISUAL_TYPE,  GLX_TRUE_COLOR,
		GLX_RED_SIZE,       8,
		GLX_GREEN_SIZE,     8,
		GLX_BLUE_SIZE,      8,
		GLX_ALPHA_SIZE,     8,
		GLX_DEPTH_SIZE,     24,
		GLX_STENCIL_SIZE,   8,
		GLX_DOUBLEBUFFER,   True,
		None
    };

    // Get framebuffer info
    sint32 fb_count;
    GLXFBConfig *fbc = glXChooseFBConfig(window->display, window->screen_id, glx_attribs, &fb_count);
    if(fbc == NULL)
    {
        printf("ERROR: Failed to retrieve framebuffer.\n");
        XCloseDisplay(window->display);
        return CONTEXT_ERROR;
    }
    
    printf("Getting best XVisualInfo.\n");
    sint32 best_fbc = -1;
    sint32 worst_fbc = -1;
    sint32 best_num_samples = -1;
    sint32 worst_num_samples = 999;
    for(int i = 0; i < fb_count; i++)
    {
		XVisualInfo *temp_vi = glXGetVisualFromFBConfig(window->display, fbc[i]);
		if(temp_vi != 0)
        {
			int samples_buf;
            int samples;

            glXGetFBConfigAttrib(window->display, fbc[i], GLX_SAMPLE_BUFFERS, &samples_buf);
			glXGetFBConfigAttrib(window->display, fbc[i], GLX_SAMPLES       , &samples);

			if(best_fbc < 0 || (samples_buf && samples > best_num_samples))
            {
				best_fbc = i;
				best_num_samples = samples;
			}

			if(worst_fbc < 0 || !samples_buf || samples < worst_num_samples)
				worst_fbc = i;

			worst_num_samples = samples;
		}

		XFree(temp_vi);
	}

    printf("Best visual info index: %d\n", best_fbc);
    GLXFBConfig glx_fb_config = fbc[best_fbc];
    XFree(fbc);
    printf("Context initialized.\n");

    // Get visual from FB config
    XVisualInfo *visual = glXGetVisualFromFBConfig(window->display, glx_fb_config);
    if(visual == NULL)
    {
        printf("ERROR: Failed to get visual from FB config.\n");
        XCloseDisplay(window->display);
        return CONTEXT_ERROR;
    }

    if(window->screen_id != visual->screen)
    {
        printf("ERROR: screen_id(%d) does not match visual->screen(%d)\n", window->screen_id, visual->screen);
        XCloseDisplay(window->display);
        return CONTEXT_ERROR;
    }

    // Set window attributes - color, pixel, etc.
    XSetWindowAttributes window_attribs;
    window_attribs.border_pixel = BlackPixel(window->display, window->screen_id);
    window_attribs.background_pixel = WhitePixel(window->display, window->screen_id);
    window_attribs.override_redirect = TRUE;
    window_attribs.colormap = XCreateColormap(window->display, 
            RootWindow(window->display, window->screen_id), visual->visual, AllocNone);
    window_attribs.event_mask = ExposureMask;

    // Create window
    window->id = XCreateWindow(
        window->display,
        RootWindow(window->display, window->screen_id),
        0,
        0,
        width,
        height, 
        0,
        visual->depth,
        InputOutput,
        visual->visual,
        CWBackPixel | CWColormap | CWBorderPixel | CWEventMask,
        &window_attribs);

    if(window->id == 0)
    {
        printf("ERROR: Failed to create window.\n");
        XCloseDisplay(window->display);
        return WINDOW_ERROR;
    }

    // Setup window delete message
    window->delete_msg = XInternAtom(window->display, "WM_DELETE_WINDOW", FALSE);
    XSetWMProtocols(window->display, window->id, &window->delete_msg, 1);

    // Disable key repeat
    XAutoRepeatOff(window->display);

    // Initialize Input system
    input_initialize();
    
    printf("Window created.\n");

    // Create GLX OpenGL Context
    glXCreateContextAttribsARBProc glXCreateContextAttribsARB = 0;
	glXCreateContextAttribsARB = (glXCreateContextAttribsARBProc) glXGetProcAddressARB((const GLubyte *) "glXCreateContextAttribsARB");
    
    const char *glx_extensions = glXQueryExtensionsString(window->display, window->screen_id);

    sint32 context_attributes[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
		GLX_CONTEXT_MINOR_VERSION_ARB, 2,
		GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
		None
    };


    window->context = 0;
    if(!isExtensionSupported(glx_extensions, "GLX_ARB_create_context"))
	    window->context = glXCreateNewContext(window->display, glx_fb_config, GLX_RGBA_TYPE, 0, TRUE);
	else
	    window->context = glXCreateContextAttribsARB(window->display, glx_fb_config, 0, TRUE, context_attributes);
    
    XSync(window->display, FALSE);

    // Verify that context is a direct context
    if(!glXIsDirect(window->display, window->context))
        printf("Indirect GLX rendering context obtained.\n");
    else
        printf("Direct GLX rendering context obtained.\n");

    // Setup window context
    glXMakeCurrent(window->display, window->id, window->context);
    
    printf("GL Vendor: %s\n", glGetString(GL_VENDOR));
    printf("GL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL Version: %s\n", glGetString(GL_VERSION));
    printf("GL Shading Language: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
        
    return OK;
}


// TODO: Add a better configuration for glx framebuffer in glx_fb_config_attribs
b8 create_xcb_window(
	PlatformHandler *platform_handler,
	const char* window_title,
	uint32 x,
	uint32 y,
	uint32 width,
	uint32 height)
{
    platform_handler->window = malloc(sizeof(WindowX11GL));
    WindowXCBGL *window = (WindowXCBGL *)platform_handler->window;

    // Open Display
    window->display = XOpenDisplay(NULL);
    if(window->display == NULL)
    {
        printf("ERROR: Failed to open display.\n");
        return WINDOW_ERROR;
    }
    
    // Setup Screen id
    window->screen_id = DefaultScreen(window->display);

    // Setup Event queue
    XSetEventQueueOwner(window->display, XCBOwnsEventQueue);

    // Setup connection
    window->xcb_connection = XGetXCBConnection(window->display);

    // Get XCB Screen
    window->xcb_screen = NULL;

    // Get data from the X server
    const struct xcb_setup_t* setup = xcb_get_setup(window->xcb_connection);

    // Loop through screens using an iterator
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    sint32 screen_p = 0;
    for (sint32 s = screen_p; s > 0; s--)
        xcb_screen_next(&it);

    // Set screen
    window->xcb_screen = it.data;
    if(window->xcb_screen == NULL)
    {
        printf("ERROR: Failed to get XCB Screen.\n");
        return WINDOW_ERROR;
    }

    // Setup GLX config attributes
    int glx_fb_config_attribs[] = {
        GLX_BUFFER_SIZE,   16,      // TODO: Check if sizes are the same
        GLX_DOUBLEBUFFER,  TRUE,
        GLX_SAMPLES,       0,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        0,
    };

    // Get FB Config
    sint32 glx_nfb_configs;
    GLXFBConfig* glx_fb_configs = glXChooseFBConfig(window->display, window->x11_id, glx_fb_config_attribs, &glx_nfb_configs);
    if(glx_nfb_configs == NULL)
    {
        printf("ERROR: Failed to choose FB config.\n");
        return CONTEXT_ERROR;
    }

    // Set FB config
    window->glx_fb_config  = glx_fb_configs[0];
    XFree(glx_fb_configs);

    // Get FB Config attributes
    sint32 glx_visual_id;
    //glXGetFBConfigAttrib(window->display, window->glx_fb_config, GLX_VISUAL_ID, &window->xcb_screen->root_visual);
    glXGetFBConfigAttrib(window->display, window->glx_fb_config, GLX_VISUAL_ID, &glx_visual_id);


    window->xcb_colormap = xcb_generate_id(window->xcb_connection);
    //xcb_create_colormap(window->xcb_connection, XCB_COLORMAP_ALLOC_NONE, window->xcb_colormap, window->xcb_screen->root, window->xcb_screen->root_visual);
    xcb_create_colormap(window->xcb_connection, XCB_COLORMAP_ALLOC_NONE, window->xcb_colormap, window->xcb_screen->root, glx_visual_id);
    
    // Setup events for window
    uint32 value_mask = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32 value_list[] = {XCB_BACK_PIXMAP_NONE, XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE
        | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE| XCB_EVENT_MASK_POINTER_MOTION 
        | XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY, window->xcb_colormap};

    // Crate XCB ID for window
    window->xcb_id = xcb_generate_id(window->xcb_connection);

    xcb_create_window(
        window->xcb_connection, 
        window->xcb_screen->root_depth, 
        window->xcb_id, 
        window->xcb_screen->root,
        0,
        0,
        width,                              //window->xcb_screen->width_in_pixels, 
        height,                             //window->xcb_screen->height_in_pixels, 
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        glx_visual_id,
        value_mask,
        value_list);

    // Map window
    xcb_map_window(window->xcb_connection, window->xcb_id);

    // Setup OpenGL configs
    sint32 glx_context_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB,   4,
        GLX_CONTEXT_MINOR_VERSION_ARB,   6,
        GLX_CONTEXT_FLAGS_ARB,           GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
        GLX_CONTEXT_PROFILE_MASK_ARB,    GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        GLX_CONTEXT_OPENGL_NO_ERROR_ARB, 1,
        0,
    };

    window->glx_id = glXCreateWindow(window->display, window->glx_fb_config, window->xcb_id, NULL);
    window->context =  glXCreateContextAttribsARB(window->display, window->glx_fb_config, NULL, 1, glx_context_attribs);

    // Setup Window properties
    xcb_change_property(
        window->xcb_connection,
        XCB_PROP_MODE_REPLACE,
        window->xcb_id,
        XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING,
        8,                      // data should be viewed 8 bits at a time, TODO: Check if sizes are the same
        strlen(window_title),
        window_title);

    // Setup messages for window
    xcb_intern_atom_cookie_t wm_delete_cookie = xcb_intern_atom(
        window->xcb_connection,
        0,
        strlen("WM_DELETE_WINDOW"),
        "WM_DELETE_WINDOW");

    xcb_intern_atom_cookie_t wm_protocols_cookie = xcb_intern_atom(
        window->xcb_connection,
        0,
        strlen("WM_PROTOCOLS"),
        "WM_PROTOCOLS");

    xcb_intern_atom_reply_t *wm_delete_reply = xcb_intern_atom_reply(
        window->xcb_connection,
        wm_delete_cookie,
        NULL);

    xcb_intern_atom_reply_t *wm_protocols_reply = xcb_intern_atom_reply(
        window->xcb_connection,
        wm_protocols_cookie,
        NULL);

    // Setup window delete message
    window->delete_msg = wm_delete_reply->atom;
    window->wm_protocols = wm_protocols_reply->atom;

    //xcb_map_window(window->xcb_connection, window->xcb_id);

    // Disable key repeat
    XAutoRepeatOff(window->display);

    // Initialize Input system
    input_initialize();
    
    // Make context current
    glXMakeContextCurrent(window->display, window->glx_id, window->glx_id, window->context);

    return OK;
}

void run_gl_xlib_window(PlatformHandler *platform_handler)
{
	WindowX11GL *window = (WindowX11GL *)platform_handler->window;
    
    // Setup Input
    XSelectInput(window->display, window->id, KeyPressMask | KeyReleaseMask 
            | KeymapStateMask   | PointerMotionMask     | ButtonPressMask 
            | ButtonReleaseMask | EnterWindowMask       | LeaveWindowMask
            | ExposureMask      | StructureNotifyMask   | ButtonPressMask
            | ButtonReleaseMask);

    // Name window
    XStoreName(window->display, window->id, "OpenGL Window Test");

    // Show window
    XClearWindow(window->display, window->id);
    XMapRaised(window->display, window->id);

    // Print window attributes
    window->window_attribs;
    XGetWindowAttributes(window->display, window->id, &window->window_attribs);
    printf("Window Info:\n");
    printf("\t%dx%d\n", window->window_attribs.width, window->window_attribs.height);

    // Resize window
    uint32 change_values = CWWidth | CWHeight;
    XWindowChanges values;
    values.width = 800;
    values.height = 600;
    XConfigureWindow(window->display, window->id, change_values, &values);

    platform_handler->running = TRUE;
}

void run_xcb_window(PlatformHandler *platform_handler)
{
    WindowXCBGL *window = (WindowXCBGL *)platform_handler->window;

    platform_handler->running = TRUE;
}

// TODO: Fix XCB window shutdown -> error in glXDestroyWindow
void shutdown_xcb_window(PlatformHandler *platform_handler)
{
    WindowXCBGL *window = (WindowXCBGL *)platform_handler->window;

    // Enable key repeat
	XAutoRepeatOn(window->display);

    xcb_destroy_window(window->xcb_connection, window->xcb_id);
    xcb_free_colormap(window->xcb_connection, window->xcb_colormap);

    glXDestroyWindow(window->display,  window->glx_id);
    glXDestroyContext(window->display, window->context);

    XCloseDisplay(window->display);
}

void shutdown_simple_window(PlatformHandler *platform_handler)
{
	WindowX11 *window = (WindowX11 *)platform_handler->window;
	
	// Enable key repeat
	XAutoRepeatOn(window->display);
	
	XCloseDisplay(window->display);
	
	if(platform_handler->window != NULL)
		free(window);
}

void shutdown_gl_xlib_window(PlatformHandler *platform_handler)
{
    WindowX11GL *window = (WindowX11GL *)platform_handler->window;

    // Enable key repeat
    XAutoRepeatOn(window->display);

    glXDestroyContext(window->display, window->context);
    glXDestroyWindow(window->display, window->id);
    
    if(platform_handler->window != NULL)
        free(window);
}

void process_gl_xlib_events(PlatformHandler *platform_handler)
{
	WindowX11GL *window = (WindowX11GL *)platform_handler->window;
	
	// Variable to read events
	XEvent event;

	// Variables to hold info about key pressed
	int len;
	char str[25] = {0};
	KeySym keysym = 0;
	slong32 msg;

	// Window loop
	b8 pressed;
	Keys key;
	
	// For key repeat detection
	XEvent next_event;
    
	XNextEvent(window->display, &event);

	switch(event.type)
	{
		case ClientMessage:
			msg = (long)window->delete_msg;
			if(event.xclient.data.l[0] == msg)
				platform_handler->running = FALSE;
			break;
		case KeyPress:
		case KeyRelease:
			len = XLookupString(&event.xkey, str, 25, &keysym, NULL);
			pressed = event.type == KeyPress;
			key = translate_keycode(keysym);
			input_process_key(key, pressed);
			input_update();
			break;
        case Expose:
            XGetWindowAttributes(window->display, window->id, &window->window_attribs);
            glViewport(0, 0, window->window_attribs.width, window->window_attribs.height);
            glClearColor(0.8f, 0.5f, 0.5f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glXSwapBuffers(window->display, window->id);
            break;
		default:
			break;
	}
}

// TODO: Fix closing event when clicking on X button
void process_xcb_events(PlatformHandler *platform_handler)
{
    WindowXCBGL *window = (WindowXCBGL *)platform_handler->window;

    // Variables to handle event
    xcb_generic_event_t *event = xcb_poll_for_event(window->xcb_connection);
    xcb_client_message_event_t *client_msg;

    // Variables to handle key press translation
    xcb_key_press_event_t *kb_event;
    xcb_keycode_t code;
    KeySym keysym = 0;

    // Variables to handle key press for LAL library
	b8 pressed;
	Keys key;

    if(event != NULL)
    {
        switch(event->response_type & ~0x80)
        {
            case XCB_CLIENT_MESSAGE:
                client_msg = (xcb_client_message_event_t *)event;
                if(client_msg->data.data32[0] == window->delete_msg)
                    platform_handler->running = FALSE;
                break;
            case XCB_KEY_PRESS:
            case XCB_KEY_RELEASE:
                kb_event = (xcb_key_press_event_t*)event;
                pressed = event->response_type == XCB_KEY_PRESS;
                code = kb_event->detail;
                keysym = XkbKeycodeToKeysym(window->display, (KeyCode)code, 0, 0);
                key = translate_keycode(keysym);
                input_process_key(key, pressed);
                input_update();
                break;
            case Expose:
                //XGetWindowAttributes(window->display, window->x11_id, &window->window_attribs);
                glClearColor(0.3f, 0.9f, 0.5f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glXSwapBuffers(window->display, window->glx_id);
                break;
            default:
                break;
        }
    }

    free(event);
    //printf("Deleted event.\n");
}

void process_simple_window_events(PlatformHandler *platform_handler)
{
	WindowX11 *window = (WindowX11 *)platform_handler->window;

	// Variable to read events
	XEvent event;

	// Variables to hold info about key pressed
	int len;
	char str[25] = {0};
	KeySym keysym = 0;
	slong32 msg;

	// Window loop
	b8 pressed;
	Keys key;
	
	// For key repeat detection
	XEvent next_event;

	XNextEvent(window->display, &event);

	switch(event.type)
	{
		case ClientMessage:
			msg = (long)window->delete_msg;
			if(event.xclient.data.l[0] == msg)
				platform_handler->running = FALSE;
			break;
		case KeyPress:
		case KeyRelease:
				len = XLookupString(&event.xkey, str, 25, &keysym, NULL);
				pressed = event.type == KeyPress;
				key = translate_keycode(keysym);
				input_process_key(key, pressed);
				input_update();
				break;
		default:
			break;
	}
}

b8 is_platform_running(PlatformHandler *platform_handler)
{
	return platform_handler->running;
}

void set_platform_running(PlatformHandler *platform_handler, b8 value)
{
	platform_handler->running = value;	
}

b8 isExtensionSupported(const char *extList, const char *extension)
{
	const char *start;
	const char *where, *terminator;

	/* Extension names should not have spaces. */
	where = strchr(extension, ' ');
	if (where || *extension == '\0')
	    return CONTEXT_ERROR;

	/* It takes a bit of care to be fool-proof about parsing the
	 OpenGL extensions string. Don't be fooled by sub-strings,
	 etc. */
	for(start=extList;;)
    {
	    where = strstr(start, extension);

	    if (!where)
	 	    break;

        terminator = where + strlen(extension);

        if( where == start || *(where - 1) == ' ' )
        {
            if ( *terminator == ' ' || *terminator == '\0' )
                return OK;
        }

        start = terminator;
	}

	return FAILED;
}

Keys translate_keycode(uint32 x_keycode) {
    switch (x_keycode) {
        case XK_BackSpace:
            return KEY_BACKSPACE;
        case XK_Return:
            return KEY_ENTER;
        case XK_Tab:
            return KEY_TAB;
        // case XK_Shift: return KEY_SHIFT;
        // case XK_Control: return KEY_CONTROL;

        case XK_Pause:
            return KEY_PAUSE;
        case XK_Caps_Lock:
            return KEY_CAPITAL;

        case XK_Escape:
            return KEY_ESCAPE;

        // Not supported
        // case : return KEY_CONVERT;
        // case : return KEY_NONCONVERT;
        // case : return KEY_ACCEPT;

        case XK_Mode_switch:
            return KEY_MODECHANGE;

        case XK_space:
            return KEY_SPACE;
        case XK_Prior:
            return KEY_PAGEUP;
        case XK_Next:
            return KEY_PAGEDOWN;
        case XK_End:
            return KEY_END;
        case XK_Home:
            return KEY_HOME;
        case XK_Left:
            return KEY_LEFT;
        case XK_Up:
            return KEY_UP;
        case XK_Right:
            return KEY_RIGHT;
        case XK_Down:
            return KEY_DOWN;
        case XK_Select:
            return KEY_SELECT;
        case XK_Print:
            return KEY_PRINT;
        case XK_Execute:
            return KEY_EXECUTE;
        // case XK_snapshot: return KEY_SNAPSHOT; // not supported
        case XK_Insert:
            return KEY_INSERT;
        case XK_Delete:
            return KEY_DELETE;
        case XK_Help:
            return KEY_HELP;

        case XK_Meta_L:
            return KEY_LSUPER;  // TODO: Not sure, will check when I have a mac :')
        case XK_Meta_R:
            return KEY_RSUPER;
        // case XK_apps: return KEY_APPS; // not supported

        // case XK_sleep: return KEY_SLEEP; //not supported

        case XK_KP_0:
            return KEY_NUMPAD0;
        case XK_KP_1:
            return KEY_NUMPAD1;
        case XK_KP_2:
            return KEY_NUMPAD2;
        case XK_KP_3:
            return KEY_NUMPAD3;
        case XK_KP_4:
            return KEY_NUMPAD4;
        case XK_KP_5:
            return KEY_NUMPAD5;
        case XK_KP_6:
            return KEY_NUMPAD6;
        case XK_KP_7:
            return KEY_NUMPAD7;
        case XK_KP_8:
            return KEY_NUMPAD8;
        case XK_KP_9:
            return KEY_NUMPAD9;
        case XK_multiply:
            return KEY_MULTIPLY;
        case XK_KP_Add:
            return KEY_ADD;
        case XK_KP_Separator:
            return KEY_SEPARATOR;
        case XK_KP_Subtract:
            return KEY_SUBTRACT;
        case XK_KP_Decimal:
            return KEY_DECIMAL;
        case XK_KP_Divide:
            return KEY_DIVIDE;
        case XK_F1:
            return KEY_F1;
        case XK_F2:
            return KEY_F2;
        case XK_F3:
            return KEY_F3;
        case XK_F4:
            return KEY_F4;
        case XK_F5:
            return KEY_F5;
        case XK_F6:
            return KEY_F6;
        case XK_F7:
            return KEY_F7;
        case XK_F8:
            return KEY_F8;
        case XK_F9:
            return KEY_F9;
        case XK_F10:
            return KEY_F10;
        case XK_F11:
            return KEY_F11;
        case XK_F12:
            return KEY_F12;
        case XK_F13:
            return KEY_F13;
        case XK_F14:
            return KEY_F14;
        case XK_F15:
            return KEY_F15;
        case XK_F16:
            return KEY_F16;
        case XK_F17:
            return KEY_F17;
        case XK_F18:
            return KEY_F18;
        case XK_F19:
            return KEY_F19;
        case XK_F20:
            return KEY_F20;
        case XK_F21:
            return KEY_F21;
        case XK_F22:
            return KEY_F22;
        case XK_F23:
            return KEY_F23;
        case XK_F24:
            return KEY_F24;

        case XK_Num_Lock:
            return KEY_NUMLOCK;
        case XK_Scroll_Lock:
            return KEY_SCROLL;

        case XK_KP_Equal:
            return KEY_NUMPAD_EQUAL;

        case XK_Shift_L:
            return KEY_LSHIFT;
        case XK_Shift_R:
            return KEY_RSHIFT;
        case XK_Control_L:
            return KEY_LCONTROL;
        case XK_Control_R:
            return KEY_RCONTROL;
        case XK_Alt_L:
            return KEY_LALT;
        case XK_Alt_R:
            return KEY_RALT;

        case XK_semicolon:
            return KEY_SEMICOLON;
        case XK_plus:
            return KEY_EQUAL;
        case XK_comma:
            return KEY_COMMA;
        case XK_minus:
            return KEY_MINUS;
        case XK_period:
            return KEY_PERIOD;
        case XK_slash:
            return KEY_SLASH;
        case XK_grave:
            return KEY_GRAVE;

        case XK_0:
            return KEY_0;
        case XK_1:
            return KEY_1;
        case XK_2:
            return KEY_2;
        case XK_3:
            return KEY_3;
        case XK_4:
            return KEY_4;
        case XK_5:
            return KEY_5;
        case XK_6:
            return KEY_6;
        case XK_7:
            return KEY_7;
        case XK_8:
            return KEY_8;
        case XK_9:
            return KEY_9;

        case XK_a:
        case XK_A:
            return KEY_A;
        case XK_b:
        case XK_B:
            return KEY_B;
        case XK_c:
        case XK_C:
            return KEY_C;
        case XK_d:
        case XK_D:
            return KEY_D;
        case XK_e:
        case XK_E:
            return KEY_E;
        case XK_f:
        case XK_F:
            return KEY_F;
        case XK_g:
        case XK_G:
            return KEY_G;
        case XK_h:
        case XK_H:
            return KEY_H;
        case XK_i:
        case XK_I:
            return KEY_I;
        case XK_j:
        case XK_J:
            return KEY_J;
        case XK_k:
        case XK_K:
            return KEY_K;
        case XK_l:
        case XK_L:
            return KEY_L;
        case XK_m:
        case XK_M:
            return KEY_M;
        case XK_n:
        case XK_N:
            return KEY_N;
        case XK_o:
        case XK_O:
            return KEY_O;
        case XK_p:
        case XK_P:
            return KEY_P;
        case XK_q:
        case XK_Q:
            return KEY_Q;
        case XK_r:
        case XK_R:
            return KEY_R;
        case XK_s:
        case XK_S:
            return KEY_S;
        case XK_t:
        case XK_T:
            return KEY_T;
        case XK_u:
        case XK_U:
            return KEY_U;
        case XK_v:
        case XK_V:
            return KEY_V;
        case XK_w:
        case XK_W:
            return KEY_W;
        case XK_x:
        case XK_X:
            return KEY_X;
        case XK_y:
        case XK_Y:
            return KEY_Y;
        case XK_z:
        case XK_Z:
            return KEY_Z;

        default:
            return 0;
    }
}

#endif // LPLATFORM_LINUX

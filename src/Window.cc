// Window.cc for Fluxbox Window Manager
// Copyright (c) 2001 - 2003 Henrik Kinnunen (fluxgen at users.sourceforge.net)
//
// Window.cc for Blackbox - an X11 Window manager
// Copyright (c) 1997 - 2000 Brad Hughes (bhughes at tcac.net)
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

// $Id: Window.cc,v 1.205 2003/07/19 11:55:49 rathnor Exp $

#include "Window.hh"

#include "WinClient.hh"
#include "I18n.hh"
#include "fluxbox.hh"
#include "Screen.hh"
#include "StringUtil.hh"
#include "Netizen.hh"
#include "FbWinFrameTheme.hh"
#include "MenuTheme.hh"
#include "TextButton.hh"
#include "EventManager.hh"
#include "FbAtoms.hh"
#include "RootTheme.hh"
#include "Workspace.hh"
#include "LayerMenu.hh"
#include "FbWinFrame.hh"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // HAVE_CONFIG_H
#ifdef SHAPE
#include <X11/extensions/shape.h>
#endif // SHAPE

//use GNU extensions
#ifndef	 _GNU_SOURCE
#define	 _GNU_SOURCE
#endif // _GNU_SOURCE

#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <cstring>
#include <cstdio>
#include <iostream>
#include <cassert>
#include <functional>
#include <algorithm>

using namespace std;

namespace {

void grabButton(Display *display, unsigned int button, 
                Window window, Cursor cursor) {

    //numlock
    XGrabButton(display, button, Mod1Mask|Mod2Mask, window, True,
                ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
                GrabModeAsync, None, cursor);
    //scrolllock
    XGrabButton(display, button, Mod1Mask|Mod5Mask, window, True,
                ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
                GrabModeAsync, None, cursor);
	
    //capslock
    XGrabButton(display, button, Mod1Mask|LockMask, window, True,
                ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
                GrabModeAsync, None, cursor);

    //capslock+numlock
    XGrabButton(display, Button1, Mod1Mask|LockMask|Mod2Mask, window, True,
                ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
                GrabModeAsync, None, cursor);

    //capslock+scrolllock
    XGrabButton(display, button, Mod1Mask|LockMask|Mod5Mask, window, True,
                ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
                GrabModeAsync, None, cursor);
	
    //capslock+numlock+scrolllock
    XGrabButton(display, button, Mod1Mask|LockMask|Mod2Mask|Mod5Mask, window, 
                True,
                ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
                GrabModeAsync, None, cursor);

    //numlock+scrollLock
    XGrabButton(display, button, Mod1Mask|Mod2Mask|Mod5Mask, window, True,
                ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
                GrabModeAsync, None, cursor);
	
}

// X event scanner for enter/leave notifies - adapted from twm
typedef struct scanargs {
    Window w;
    Bool leave, inferior, enter;
} scanargs;

// look for valid enter or leave events (that may invalidate the earlier one we are interested in)
static Bool queueScanner(Display *, XEvent *e, char *args) {
    if (e->type == LeaveNotify &&
        e->xcrossing.window == ((scanargs *) args)->w &&
        e->xcrossing.mode == NotifyNormal) {
        ((scanargs *) args)->leave = true;
        ((scanargs *) args)->inferior = (e->xcrossing.detail == NotifyInferior);
    } else if (e->type == EnterNotify &&
               e->xcrossing.mode == NotifyUngrab)
        ((scanargs *) args)->enter = true;

    return false;
}

/// returns the deepest transientFor, asserting against a close loop
WinClient *getRootTransientFor(WinClient *client) {
    while (client->transientFor()) {
        assert(client != client->transientFor());
        client = client->transientFor();
    }
    return client;
}


/// raise window and do the same for each transient of the current window
void raiseFluxboxWindow(FluxboxWindow &win) {
    if (win.oplock) return;
    win.oplock = true;

    if (!win.isIconic()) {
        win.screen().updateNetizenWindowRaise(win.clientWindow());
        win.layerItem().raise();
    }

    // for each transient do raise
    WinClient::TransientList::const_iterator it = win.winClient().transientList().begin();
    WinClient::TransientList::const_iterator it_end = win.winClient().transientList().end();
    for (; it != it_end; ++it) {
        if ((*it)->fbwindow() && !(*it)->fbwindow()->isIconic())
            // TODO: should we also check if it is the active client?
            raiseFluxboxWindow(*(*it)->fbwindow());
    }
    win.oplock = false;
}

/// lower window and do the same for each transient it holds
void lowerFluxboxWindow(FluxboxWindow &win) {
    if (win.oplock) return;
    win.oplock = true;

    if (!win.isIconic()) {
        win.screen().updateNetizenWindowLower(win.clientWindow());
        win.layerItem().lower();
    }

    WinClient::TransientList::const_iterator it = win.winClient().transientList().begin();
    WinClient::TransientList::const_iterator it_end = win.winClient().transientList().end();
    for (; it != it_end; ++it) {
        if ((*it)->fbwindow() && !(*it)->fbwindow()->isIconic())
            // TODO: should we also check if it is the active client?
            lowerFluxboxWindow(*(*it)->fbwindow());
    }
    win.oplock = false;
}

/// raise window and do the same for each transient it holds
void tempRaiseFluxboxWindow(FluxboxWindow &win) {
    if (win.oplock) return;
    win.oplock = true;

    if (!win.isIconic()) {
        // don't update netizen, as it is only temporary
        win.layerItem().tempRaise();
    }

    // for each transient do raise
    WinClient::TransientList::const_iterator it = win.winClient().transientList().begin();
    WinClient::TransientList::const_iterator it_end = win.winClient().transientList().end();
    for (; it != it_end; ++it) {
        if ((*it)->fbwindow() && !(*it)->fbwindow()->isIconic())
            // TODO: should we also check if it is the active client?
            tempRaiseFluxboxWindow(*(*it)->fbwindow());
    }
    win.oplock = false;
}

class SetClientCmd:public FbTk::Command {
public:
    explicit SetClientCmd(WinClient &client):m_client(client) {
    }
    void execute() {
        if (m_client.m_win != 0)
            m_client.m_win->setCurrentClient(m_client);
    }
private:
    WinClient &m_client;
};

};

template <>
void LayerMenuItem<FluxboxWindow>::click(int button, int time) {
    m_object->moveToLayer(m_layernum);
}

FluxboxWindow::FluxboxWindow(WinClient &client, BScreen &scr, FbWinFrameTheme &tm,
                             FbTk::XLayer &layer):
    oplock(false),
    m_hintsig(*this),
    m_statesig(*this),
    m_layersig(*this),
    m_workspacesig(*this),
    m_diesig(*this),
    moving(false), resizing(false), shaded(false), 
    iconic(false), focused(false),
    stuck(false), m_managed(false),
    maximized(MAX_NONE),
    m_screen(scr),
    m_timer(this),
    display(0),
    m_layermenu(new LayerMenu<FluxboxWindow>(*scr.menuTheme(), 
                                             scr.screenNumber(),
                                             scr.imageControl(), 
                                             *scr.layerManager().getLayer(Fluxbox::instance()->getMenuLayer()), 
                                             this,
                                             false)),
    m_windowmenu(*scr.menuTheme(), scr.screenNumber(), scr.imageControl()),
    m_old_decoration(DECOR_NORMAL),
    m_client(&client),   
    m_frame(new FbWinFrame(tm, scr.imageControl(), scr.screenNumber(), 0, 0, 100, 100)),
    m_strut(0),
    m_layeritem(m_frame->window(), layer),
    m_layernum(layer.getLayerNum()),
    m_parent(scr.rootWindow()) {

    init();
}


FluxboxWindow::~FluxboxWindow() {
#ifdef DEBUG
    cerr<<__FILE__<<"("<<__LINE__<<"): starting ~FluxboxWindow("<<this<<")"<<endl;
    cerr<<__FILE__<<"("<<__LINE__<<"): num clients = "<<numClients()<<endl;
    cerr<<__FILE__<<"("<<__LINE__<<"): curr client = "<<m_client<<endl;
    cerr<<__FILE__<<"("<<__LINE__<<"): m_labelbuttons.size = "<<m_labelbuttons.size()<<endl;
#endif // DEBUG

    clearStrut();

    if (moving || resizing || m_attaching_tab) {
        screen().hideGeometry();
        XUngrabPointer(display, CurrentTime);
    }

    // no longer a valid window to do stuff with
    Fluxbox::instance()->removeWindowSearch(frame().window().window());

    Client2ButtonMap::iterator it = m_labelbuttons.begin();
    Client2ButtonMap::iterator it_end = m_labelbuttons.end();
    for (; it != it_end; ++it) {
        frame().removeLabelButton(*(*it).second);
        delete (*it).second;
    }
    m_labelbuttons.clear();

    m_timer.stop();
    
    // notify die
    m_diesig.notify();

    if (m_client != 0)
        delete m_client; // this also removes client from our list
    m_client = 0;

    if (m_clientlist.size() > 1) {
        cerr<<__FILE__<<"("<<__FUNCTION__<<") WARNING! clientlist > 1"<<endl;
        while (!m_clientlist.empty()) {
            detachClient(*m_clientlist.back());
        }
    }

#ifdef DEBUG
    cerr<<__FILE__<<"("<<__LINE__<<"): ~FluxboxWindow("<<this<<")"<<endl;
#endif // DEBUG
}


void FluxboxWindow::init() { 
    // so parent menu don't kill us
    m_layermenu->setInternalMenu();
    m_layermenu->disableTitle();

    m_attaching_tab = 0;

    assert(m_client);
    m_client->m_win = this;
    m_client->setGroupLeftWindow(None); // nothing to the left.

    // check for shape extension and whether the window is shaped
    m_shaped = false;
#ifdef SHAPE
    if (Fluxbox::instance()->haveShape()) {
        Display *disp = FbTk::App::instance()->display();
        int not_used;
        unsigned int not_used2;
        int shaped;
        XShapeSelectInput(disp, m_client->window(), ShapeNotifyMask);
        XShapeQueryExtents(disp, m_client->window(), 
                           &shaped,  /// bShaped
                           &not_used, &not_used,  // xbs, ybs
                           &not_used2, &not_used2, // wbs, hbs
                           &not_used, // cShaped
                           &not_used, &not_used, // xcs, ycs
                           &not_used2, &not_used2); // wcs, hcs
        m_shaped = (shaped != 0 ? true : false);
    }
#endif // SHAPE

    //!! TODO init of client should be better
    // we don't want to duplicate code here and in attachClient
    m_clientlist.push_back(m_client);
#ifdef DEBUG
    cerr<<__FILE__<<": FluxboxWindow::init(this="<<this<<", client="<<hex<<
        m_client->window()<<", frame = "<<frame().window().window()<<dec<<")"<<endl;

#endif // DEBUG    

    Fluxbox &fluxbox = *Fluxbox::instance();

    // setup cursors for resize grips
    frame().gripLeft().setCursor(frame().theme().lowerLeftAngleCursor());
    frame().gripRight().setCursor(frame().theme().lowerRightAngleCursor());

    frame().resize(m_client->width(), m_client->height());
    TextButton *btn =  new TextButton(frame().label(), 
                                      frame().theme().font(),
                                      m_client->title());
    btn->setJustify(frame().theme().justify());
    m_labelbuttons[m_client] = btn;
    frame().addLabelButton(*btn);
    frame().setLabelButtonFocus(*btn);
    btn->show();    
    FbTk::EventManager &evm = *FbTk::EventManager::instance();
    // we need motion notify so we mask it
    btn->window().setEventMask(ExposureMask | ButtonPressMask | ButtonReleaseMask | 
                               ButtonMotionMask | EnterWindowMask);

    FbTk::RefCount<FbTk::Command> set_client_cmd(new SetClientCmd(*m_client));
    btn->setOnClick(set_client_cmd);
    evm.add(*this, btn->window()); // we take care of button events for this
    evm.add(*this, m_client->window());

    // redirect events from frame to us

    frame().setEventHandler(*this); 

    m_last_focus_time.tv_sec = m_last_focus_time.tv_usec = 0;

    // display connection
    display = FbTk::App::instance()->display();

    m_blackbox_attrib.workspace = m_workspace_number = ~0;

    m_blackbox_attrib.flags = m_blackbox_attrib.attrib = m_blackbox_attrib.stack = 0;
    m_blackbox_attrib.premax_x = m_blackbox_attrib.premax_y = 0;
    m_blackbox_attrib.premax_w = m_blackbox_attrib.premax_h = 0;

    //use tab as default
    decorations.tab = true;
    // enable decorations
    decorations.enabled = true;

    // set default values for decoration
    decorations.menu = true;	//override menu option
    // all decorations on by default
    decorations.titlebar = decorations.border = decorations.handle = true;
    decorations.maximize = decorations.close = 
        decorations.sticky = decorations.shade = decorations.tab = true;


    functions.resize = functions.move = functions.iconify = functions.maximize = true;
    functions.close = decorations.close = false;

    if (m_client->getBlackboxHint() != 0)
        getBlackboxHints();
    else
        getMWMHints();
    
    // get size, aspect, minimum/maximum size and other hints set
    // by the client

    getWMProtocols();
    if (m_client->window_group != None) 
        Fluxbox::instance()->saveGroupSearch(m_client->window_group, this);

    //!!
    // fetch client size and placement
    XWindowAttributes wattrib;
    if (! m_client->getAttrib(wattrib) ||
        !wattrib.screen // no screen? ??
        || wattrib.override_redirect) { // override redirect        
        return;
    }

    // save old border width so we can restore it later
    m_client->old_bw = wattrib.border_width;
    m_client->x = wattrib.x; m_client->y = wattrib.y;

    fluxbox.saveWindowSearch(frame().window().window(), this);

    m_timer.setTimeout(fluxbox.getAutoRaiseDelay());
    m_timer.fireOnce(true);

    if (m_client->initial_state == WithdrawnState) {
        return;
    }

    m_managed = true; //this window is managed
	
    // update transient infomation
    m_client->updateTransientInfo();
	
    // adjust the window decorations based on transience and window sizes
    if (m_client->isTransient()) {
        decorations.maximize =  functions.maximize = false;
        decorations.handle = false;
    }	
	
    if ((m_client->normal_hint_flags & PMinSize) &&
        (m_client->normal_hint_flags & PMaxSize) &&
        m_client->max_width != 0 && m_client->max_width <= m_client->min_width &&
        m_client->max_height != 0 && m_client->max_height <= m_client->min_height) {
        decorations.maximize = decorations.handle =
            functions.resize = functions.maximize = false;
        decorations.tab = false; //no tab for this window
    }

    upsize();

    bool place_window = true;
    if (fluxbox.isStartup() || m_client->isTransient() ||
        m_client->normal_hint_flags & (PPosition|USPosition)) {
        setGravityOffsets();

        if (! fluxbox.isStartup()) {

            int real_x = frame().x();
            int real_y = frame().y();

            if (real_x >= 0 && 
                real_y + frame().y() >= 0 &&
                real_x <= (signed) screen().width() &&
                real_y <= (signed) screen().height())
                place_window = false;

        } else
            place_window = false;

    }

    associateClientWindow();

    grabButtons();
		
    applyDecorations();

    if (m_workspace_number < 0 || m_workspace_number >= screen().getCount())
        m_workspace_number = screen().currentWorkspaceID();

    restoreAttributes();

    frame().move(wattrib.x, wattrib.y);
    frame().resizeForClient(wattrib.width, wattrib.height);

    // if we're a transient then we should be on the same layer as our parent
    if (m_client->isTransient() && 
        m_client->transientFor()->fbwindow() &&
        m_client->transientFor()->fbwindow() != this)
        layerItem().setLayer(m_client->transientFor()->fbwindow()->layerItem().getLayer());       
    else // if no parent then set default layer
        moveToLayer(m_layernum);
    
    if (!place_window)
        moveResize(frame().x(), frame().y(), frame().width(), frame().height());

    screen().getWorkspace(m_workspace_number)->addWindow(*this, place_window);

    if (shaded) { // start shaded
        shaded = false;
        shade();
    }

    if (maximized && functions.maximize) { // start maximized
        maximized = MAX_NONE;
        maximize();
    }	

    if (stuck) {
        stuck = false;
        stick();
        deiconify(); //we're omnipresent and visible
    }

    setState(m_current_state);
    frame().reconfigure();
    sendConfigureNotify();
    // no focus default
    setFocusFlag(false);

    if (m_shaped)
        shape();
}

/// apply shape to this window
void FluxboxWindow::shape() {
#ifdef SHAPE
    if (m_shaped) {
        Display *disp = FbTk::App::instance()->display();
        XShapeCombineShape(disp,
                           frame().window().window(), ShapeBounding,
                           0, frame().clientArea().y(), // xOff, yOff
                           m_client->window(),
                           ShapeBounding, ShapeSet);
        XFlush(disp);
    }
#endif // SHAPE

}


/// attach a client to this window and destroy old window
void FluxboxWindow::attachClient(WinClient &client) {
    //!! TODO: check for isGroupable in client
    if (client.m_win == this)
        return;

    // reparent client win to this frame 
    frame().setClientWindow(client);
    FbTk::EventManager &evm = *FbTk::EventManager::instance();

    // get the current window on the end of our client list
    Window leftwin = None;
    if (!clientList().empty())
        leftwin = clientList().back()->window();

    client.setGroupLeftWindow(leftwin);

    if (client.fbwindow() != 0) {
        FluxboxWindow *old_win = client.fbwindow(); // store old window

        Fluxbox *fb = Fluxbox::instance();
        // make sure we set new window search for each client
        ClientList::iterator client_it = old_win->clientList().begin();
        ClientList::iterator client_it_end = old_win->clientList().end();
        for (; client_it != client_it_end; ++client_it) {
            // setup eventhandlers for client
            fb->saveWindowSearch((*client_it)->window(), this);
            evm.add(*this, (*client_it)->window());
            
            // reparent window to this
            frame().setClientWindow(**client_it);
            resizeClient(**client_it, 
                         frame().clientArea().width(),
                         frame().clientArea().height());

            (*client_it)->m_win = this;
            // create a labelbutton for this client and 
            // associate it with the pointer
            TextButton *btn = new TextButton(frame().label(), 
                                             frame().theme().font(),
                                             (*client_it)->title());
            btn->setJustify(frame().theme().justify());
            m_labelbuttons[(*client_it)] = btn;
            frame().addLabelButton(*btn);
            btn->show();
            // we need motion notify so we mask it
            btn->window().setEventMask(ExposureMask | ButtonPressMask | 
                                       ButtonReleaseMask | ButtonMotionMask | 
                                       EnterWindowMask);


            FbTk::RefCount<FbTk::Command> 
                set_client_cmd(new SetClientCmd(*(*client_it)));
            btn->setOnClick(set_client_cmd);
            evm.add(*this, btn->window()); // we take care of button events for this

            (*client_it)->saveBlackboxAttribs(m_blackbox_attrib);
        }

        // add client and move over all attached clients 
        // from the old window to this list
        // all the "left window"s will remain the same, except for the first.
        m_clientlist.splice(m_clientlist.end(), old_win->m_clientlist);           
        old_win->m_client = 0;

        delete old_win;
        
    } else { // client.fbwindow() == 0
        // create a labelbutton for this client and associate it with the pointer
        TextButton *btn = new TextButton(frame().label(), 
                                         frame().theme().font(),
                                         client.title());
        m_labelbuttons[&client] = btn;
        frame().addLabelButton(*btn);
        btn->show();
        FbTk::EventManager &evm = *FbTk::EventManager::instance();
        // we need motion notify so we mask it
        btn->window().setEventMask(ExposureMask | ButtonPressMask | 
                                   ButtonReleaseMask | ButtonMotionMask | 
                                   EnterWindowMask);


        FbTk::RefCount<FbTk::Command> set_client_cmd(new SetClientCmd(client));
        btn->setOnClick(set_client_cmd);
        evm.add(*this, btn->window()); // we take care of button events for this

        client.m_win = this;    

        Fluxbox::instance()->saveWindowSearch(client.window(), this);
        client.saveBlackboxAttribs(m_blackbox_attrib);
        m_clientlist.push_back(&client);
    }

    // make sure that the state etc etc is updated for the new client
    // TODO: one day these should probably be neatened to only act on the
    // affected clients if possible
    m_statesig.notify();
    m_workspacesig.notify();
    m_layersig.notify();

    frame().reconfigure();

    // keep the current window on top
    m_client->raise();
}


/// detach client from window and create a new window for it
bool FluxboxWindow::detachClient(WinClient &client) {
    if (client.m_win != this || numClients() <= 1)
        return false;
    
    // I'm not sure how to do this bit better
    // we need to find the window we've got, and update the
    // window to its right to have a left window set to the
    // window which is to the left of the current.
    // Think in terms of:
    // window1 <- my_window <- window2
    // we need to take out my_window, so update window2 leftwin to be window1

    Window leftwin = None;
    ClientList::iterator client_it_end = clientList().end();
    ClientList::iterator client_it = clientList().begin();
    ClientList::iterator client_it_before = client_it_end;
    ClientList::iterator client_it_after = clientList().begin();
    if (!clientList().empty()) {
        ++client_it_after;
        if (clientList().front() == &client) {
            leftwin = None;
        } else {
            ++client_it;
            client_it_before = clientList().begin();
            ++client_it_after;

            while (client_it != client_it_end) {
                if (*client_it == &client) {
                    break;
                }
                ++client_it_before;
                ++client_it;
                ++client_it_after;
            }
        }
    }

    // update the leftwin of the window to the right
    if (client_it_before != client_it_end) 
        leftwin = (*client_it_before)->window();

    if (client_it_after != client_it_end)
        (*client_it_after)->setGroupLeftWindow(leftwin);

    removeClient(client);

    // m_client must be valid as there should be at least one other window 
    // otherwise this wouldn't be here (refer numClients() <= 1 return)
    m_client->raise();
    setInputFocus();
    return true;
}

void FluxboxWindow::detachCurrentClient() {
    // should only operate if we had more than one client
    if (numClients() <= 1)
        return;
    detachClient(*m_client);
}

/// removes client from client list, does not create new fluxboxwindow for it
bool FluxboxWindow::removeClient(WinClient &client) {
    if (client.m_win != this || numClients() == 0)
        return false;

#ifdef DEBUG
    cerr<<__FILE__<<"("<<__FUNCTION__<<")["<<this<<"]"<<endl;
#endif // DEBUG
    
    // if it is our active client, deal with it...
    if (m_client == &client) {
    // set next client to be focused
    // if the client we're about to remove is the last client then set prev client
        if (&client == m_clientlist.back())
            prevClient();
        else
            nextClient();
    }

    client.m_win = 0;
    m_clientlist.remove(&client);

    if (m_client == &client) {
        if (m_clientlist.empty())
            m_client = 0;
        else
            // this really shouldn't happen
            m_client = m_clientlist.back();
    }

    FbTk::EventManager &evm = *FbTk::EventManager::instance();
    evm.remove(client.window());

    FbTk::Button *label_btn = m_labelbuttons[&client];
    if (label_btn != 0) {
        frame().removeLabelButton(*label_btn);
        evm.remove(label_btn->window());
        delete label_btn;
        label_btn = 0;
    }

    m_labelbuttons.erase(&client);

#ifdef DEBUG
    cerr<<__FILE__<<"("<<__FUNCTION__<<")["<<this<<"] numClients = "<<numClients()<<endl;
#endif // DEBUG   

    return true;
}

/// returns WinClient of window we're searching for
WinClient *FluxboxWindow::findClient(Window win) {
    std::list<WinClient *>::iterator it = m_clientlist.begin();
    std::list<WinClient *>::iterator it_end = m_clientlist.end();
    for (; it != it_end; ++it) {
        if ((*it)->window() == win)
            return (*it);
    }
    // failure
    return 0;
}

/// raise and focus next client
void FluxboxWindow::nextClient() {
    if (numClients() <= 1)
        return;

    ClientList::iterator it = find(m_clientlist.begin(), m_clientlist.end(), m_client);
    if (it == m_clientlist.end()) {
        m_client = m_clientlist.front();
        return;
    }

    it++;
    if (it == m_clientlist.end())
        m_client = m_clientlist.front();
    else
        m_client = *it;
    m_client->raise();
    frame().setLabelButtonFocus(*m_labelbuttons[m_client]);
    setInputFocus();
}

void FluxboxWindow::prevClient() {
    if (numClients() <= 1)
        return;

    ClientList::iterator it = find(m_clientlist.begin(), m_clientlist.end(), m_client);
    if (it == m_clientlist.end()) {
        m_client = m_clientlist.front();
        return;
    }
    if (it == m_clientlist.begin())
        m_client = m_clientlist.back();
    else
        m_client = *(--it);

    m_client->raise();
    frame().setLabelButtonFocus(*m_labelbuttons[m_client]);
    setInputFocus();
}

bool FluxboxWindow::setCurrentClient(WinClient &client, bool setinput) {
    // make sure it's in our list
    if (client.m_win != this)
        return false;

    m_client = &client;
    m_client->raise();
    frame().setLabelButtonFocus(*m_labelbuttons[m_client]);
    return setinput && setInputFocus();
}

bool FluxboxWindow::isGroupable() const {
    if (isResizable() && isMaximizable() && !winClient().isTransient())
        return true;
    return false;
}

void FluxboxWindow::associateClientWindow() {
    m_client->setBorderWidth(0);
    updateTitleFromClient();
    updateIconNameFromClient();

    frame().setClientWindow(*m_client);
    frame().resizeForClient(m_client->width(), m_client->height());
    // make sure the frame reconfigures
    frame().reconfigure();
}


void FluxboxWindow::grabButtons() {

    XGrabButton(display, Button1, AnyModifier, 
		frame().clientArea().window(), True, ButtonPressMask,
		GrabModeSync, GrabModeSync, None, None);		
    XUngrabButton(display, Button1, Mod1Mask|Mod2Mask|Mod3Mask, frame().clientArea().window());


    XGrabButton(display, Button1, Mod1Mask, frame().window().window(), True,
		ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
		GrabModeAsync, None, frame().theme().moveCursor());

    //----grab with "all" modifiers
    grabButton(display, Button1, frame().window().window(), frame().theme().moveCursor());
	
    XGrabButton(display, Button2, Mod1Mask, frame().window().window(), True,
		ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, None);
		
    XGrabButton(display, Button3, Mod1Mask, frame().window().window(), True,
		ButtonReleaseMask | ButtonMotionMask, GrabModeAsync,
		GrabModeAsync, None, frame().theme().lowerRightAngleCursor());
	
    //---grab with "all" modifiers
    grabButton(display, Button3, frame().window().window(), frame().theme().lowerRightAngleCursor());
}


void FluxboxWindow::reconfigure() {

    upsize();

    applyDecorations();

    setFocusFlag(focused);

    moveResize(frame().x(), frame().y(), frame().width(), frame().height());
	
    grabButtons();

    frame().setDoubleClickTime(Fluxbox::instance()->getDoubleClickInterval());

    m_windowmenu.reconfigure();
	
}

/// update current client title and title in our frame
void FluxboxWindow::updateTitleFromClient() {

    m_client->updateTitle();
    m_labelbuttons[m_client]->setText(m_client->title());    
    m_labelbuttons[m_client]->clear(); // redraw text
}

/// update icon title from client
void FluxboxWindow::updateIconNameFromClient() {
    m_client->updateIconTitle();
}


void FluxboxWindow::getWMProtocols() {
    Atom *proto = 0;
    int num_return = 0;
    FbAtoms *fbatoms = FbAtoms::instance();

    if (XGetWMProtocols(display, m_client->window(), &proto, &num_return)) {

        for (int i = 0; i < num_return; ++i) {
            if (proto[i] == fbatoms->getWMDeleteAtom())
                functions.close = true;
            else if (proto[i] == fbatoms->getWMTakeFocusAtom())
                m_client->send_focus_message = true;
            else if (proto[i] == fbatoms->getFluxboxStructureMessagesAtom())
                screen().addNetizen(m_client->window());
        }

        XFree(proto);
    } else {
        cerr<<"Warning: Failed to read WM Protocols. "<<endl;
    }

}


void FluxboxWindow::getMWMHints() {
    const WinClient::MwmHints *hint = m_client->getMwmHint();

    if (!hint) return;

    if (hint->flags & MwmHintsDecorations) {
        if (hint->decorations & MwmDecorAll) {
            decorations.titlebar = decorations.handle = decorations.border =
                decorations.iconify = decorations.maximize =
                decorations.close = decorations.menu = true;
        } else {
            decorations.titlebar = decorations.handle = decorations.border =
                decorations.iconify = decorations.maximize =
                decorations.close = decorations.tab = false;
            decorations.menu = true;
            if (hint->decorations & MwmDecorBorder)
                decorations.border = true;
            if (hint->decorations & MwmDecorHandle)
                decorations.handle = true;
            if (hint->decorations & MwmDecorTitle) {                
                //only tab on windows with titlebar
                decorations.titlebar = decorations.tab = true;
            }
            
            if (hint->decorations & MwmDecorMenu)
                decorations.menu = true;
            if (hint->decorations & MwmDecorIconify)
                decorations.iconify = true;
            if (hint->decorations & MwmDecorMaximize)
                decorations.maximize = true;
        }
    }
	
    if (hint->flags & MwmHintsFunctions) {
        if (hint->functions & MwmFuncAll) {
            functions.resize = functions.move = functions.iconify =
                functions.maximize = functions.close = true;
        } else {
            functions.resize = functions.move = functions.iconify =
                functions.maximize = functions.close = false;

            if (hint->functions & MwmFuncResize)
                functions.resize = true;
            if (hint->functions & MwmFuncMove)
                functions.move = true;
            if (hint->functions & MwmFuncIconify)
                functions.iconify = true;
            if (hint->functions & MwmFuncMaximize)
                functions.maximize = true;
            if (hint->functions & MwmFuncClose)
                functions.close = true;
        }
    }
	
	
}


void FluxboxWindow::getBlackboxHints() {
    const FluxboxWindow::BlackboxHints *hint = m_client->getBlackboxHint();
    if (!hint) return;

    if (hint->flags & ATTRIB_SHADED)
        shaded = (hint->attrib & ATTRIB_SHADED);

    if ((hint->flags & ATTRIB_MAXHORIZ) &&
        (hint->flags & ATTRIB_MAXVERT))
        maximized = ((hint->attrib &
                      (ATTRIB_MAXHORIZ | 
                       ATTRIB_MAXVERT)) ? MAX_FULL : MAX_NONE);
    else if (hint->flags & ATTRIB_MAXVERT)
        maximized = ((hint->attrib & 
                      ATTRIB_MAXVERT) ? MAX_VERT : MAX_NONE);
    else if (hint->flags & ATTRIB_MAXHORIZ)
        maximized = ((hint->attrib & 
                      ATTRIB_MAXHORIZ) ? MAX_HORZ : MAX_NONE);

    if (hint->flags & ATTRIB_OMNIPRESENT)
        stuck = (hint->attrib & 
                 ATTRIB_OMNIPRESENT);

    if (hint->flags & ATTRIB_WORKSPACE)
        m_workspace_number = hint->workspace;

    if (hint->flags & ATTRIB_STACK)
        m_workspace_number = hint->stack;

    if (hint->flags & ATTRIB_DECORATION) {
        m_old_decoration = static_cast<Decoration>(hint->decoration);
        setDecoration(m_old_decoration);
    }
}

void FluxboxWindow::move(int x, int y) {
    moveResize(x, y, frame().width(), frame().height());
}

void FluxboxWindow::resize(unsigned int width, unsigned int height) {
    moveResize(frame().x(), frame().y(), width, height);
}

void FluxboxWindow::moveResize(int new_x, int new_y,
                               unsigned int new_width, unsigned int new_height) {

    bool send_event = (frame().x() != new_x || frame().y() != new_y);

    if (new_width != frame().width() || new_height != frame().height()) {
        if ((((signed) frame().width()) + new_x) < 0) 
            new_x = 0;
        if ((((signed) frame().height()) + new_y) < 0) 
            new_y = 0;

        downsize();
        if (!isResizable()) {
            new_width = width();
            new_height = height();
        }
        frame().moveResize(new_x, new_y, new_width, new_height);

        setFocusFlag(focused);
        shaded = false;
        send_event = true;
    } else {
        frame().move(new_x, new_y);
		
        send_event = true;
    }

    if (send_event && ! moving) {
        sendConfigureNotify();
    }

    shape();
}	

bool FluxboxWindow::setInputFocus() {

    //TODO hint skip focus
    if (((signed) (frame().x() + frame().width())) < 0) {
        if (((signed) (frame().y() + frame().height())) < 0) {
            moveResize(frame().window().borderWidth(), frame().window().borderWidth(),
                       frame().width(), frame().height());
        } else if (frame().y() > (signed) screen().height()) {
            moveResize(frame().window().borderWidth(), screen().height() - frame().height(),
                       frame().width(), frame().height());
        } else {
            moveResize(frame().window().borderWidth(), frame().y() + frame().window().borderWidth(),
                       frame().width(), frame().height());
        }
    } else if (frame().x() > (signed) screen().width()) {
        if (((signed) (frame().y() + frame().height())) < 0) {
            moveResize(screen().width() - frame().width(), frame().window().borderWidth(),
                       frame().width(), frame().height());
        } else if (frame().y() > (signed) screen().height()) {
            moveResize(screen().width() - frame().width(),
                       screen().height() - frame().height(), 
                       frame().width(), frame().height());
        } else {
            moveResize(screen().width() - frame().width(),
                       frame().y() + frame().window().borderWidth(), 
                       frame().width(), frame().height());
        }
    }

    if (! validateClient())
        return false;

    bool ret = false;

    if (!m_client->transients.empty() && m_client->isModal()) {
        WinClient::TransientList::iterator it = m_client->transients.begin();
        WinClient::TransientList::iterator it_end = m_client->transients.end();
        for (; it != it_end; ++it) {
            if ((*it)->isModal())
                return (*it)->fbwindow()->setCurrentClient(**it, true);
        }
    } else {
        if (m_client->getFocusMode() == WinClient::F_LOCALLYACTIVE ||
            m_client->getFocusMode() == WinClient::F_PASSIVE) {
            m_client->setInputFocus(RevertToPointerRoot, CurrentTime);
        } else {
            return false;
        }

	screen().setFocusedWindow(*m_client);

        Fluxbox::instance()->setFocusedWindow(this);

        frame().setFocus(true);

        m_client->sendFocus();

        if ((screen().isSloppyFocus() || screen().isSemiSloppyFocus())
            && screen().doAutoRaise())
            m_timer.start();

        ret = true;
    }

    return ret;
}

void FluxboxWindow::hide() {
#ifdef DEBUG
    cerr<<__FILE__<<"("<<__FUNCTION__<<")["<<this<<"]"<<endl;
#endif // DEBUG
    m_windowmenu.hide();
    frame().hide();
}

void FluxboxWindow::show() {
    frame().show();
}

/**
   Unmaps the window and removes it from workspace list
*/
void FluxboxWindow::iconify() {
    if (isIconic()) // no need to iconify if we're already
        return;

    m_windowmenu.hide();
    iconic = true;

    setState(IconicState);

    frame().hide();

    ClientList::iterator client_it = m_clientlist.begin();
    const ClientList::iterator client_it_end = m_clientlist.end();
    for (; client_it != client_it_end; ++client_it) {
        WinClient &client = *(*client_it);
        client.setEventMask(NoEventMask);
        client.hide();
        client.setEventMask(PropertyChangeMask | StructureNotifyMask | FocusChangeMask);
        if (client.transientFor() &&
            client.transientFor()->fbwindow()) {
            if (!client.transientFor()->fbwindow()->isIconic()) {
                client.transientFor()->fbwindow()->iconify();
            }
        }

        if (!client.transientList().empty()) {
            WinClient::TransientList::iterator it = client.transientList().begin();
            WinClient::TransientList::iterator it_end = client.transientList().end();
            for (; it != it_end; it++)
                if ((*it)->fbwindow()) 
                    (*it)->fbwindow()->iconify();
        }
    }
    if (Fluxbox::instance()->getFocusedWindow() == this) 
        Fluxbox::instance()->revertFocus(screen());

}

void FluxboxWindow::deiconify(bool reassoc, bool do_raise) {
    if (numClients() == 0)
        return;

    if (oplock) return;
    oplock = true;

    if (iconic || reassoc) {
        screen().reassociateWindow(this, screen().currentWorkspace()->workspaceID(), false);
    } else if (moving || m_workspace_number != screen().currentWorkspace()->workspaceID()) {
        oplock = false;
        return;
    }

    bool was_iconic = iconic;

    iconic = false;
    setState(NormalState);

    ClientList::iterator client_it = clientList().begin();
    ClientList::iterator client_it_end = clientList().end();
    for (; client_it != client_it_end; ++client_it) {
        (*client_it)->setEventMask(NoEventMask);        
        (*client_it)->show();
        (*client_it)->setEventMask(PropertyChangeMask | StructureNotifyMask | FocusChangeMask);
    }

    frame().show();

    if (was_iconic && screen().doFocusNew())
        setInputFocus();

    if (focused != frame().focused())
        frame().setFocus(focused);


    if (reassoc && !m_client->transients.empty()) {
        // deiconify all transients
        client_it = clientList().begin();
        for (; client_it != client_it_end; ++client_it) {
            //TODO: Can this get stuck in a loop?
            WinClient::TransientList::iterator trans_it = 
                (*client_it)->transientList().begin();
            WinClient::TransientList::iterator trans_it_end = 
                (*client_it)->transientList().end();
            for (; trans_it != trans_it_end; ++trans_it) {
                if ((*trans_it)->fbwindow())
                    (*trans_it)->fbwindow()->deiconify(true, false);
            }
        }
    }
    oplock = false;
    if (do_raise)
	raise();
}

/**
   Send close request to client window
*/
void FluxboxWindow::close() {
#ifdef DEBUG
    cerr<<__FILE__<<"("<<__FUNCTION__<<")"<<endl;
#endif // DEBUG    
    m_client->sendClose();
}

/**
 Set window in withdrawn state
*/
void FluxboxWindow::withdraw() {
    iconic = false;

    if (isResizing())
        stopResizing();

    frame().hide();

    m_windowmenu.hide();
}

/**
   Maximize window both horizontal and vertical
*/
void FluxboxWindow::maximize() {
    if (isIconic())
        deiconify();

    if (!maximized) {
        // save old values
        int head = screen().getHead(frame().window());
        m_old_width = frame().width();
        m_old_height = frame().height();
        m_old_pos_x = frame().x();
        m_old_pos_y = frame().y();
        unsigned int left_x = screen().maxLeft(head);
        unsigned int max_width = screen().maxRight(head);
        unsigned int max_top = screen().maxTop(head);
        moveResize(left_x, max_top, 
                   max_width - left_x - 2*frame().window().borderWidth(), 
                   screen().maxBottom(head) - max_top - 2*frame().window().borderWidth());
        maximized = MAX_FULL;
    } else { // demaximize, restore to old values
        moveResize(m_old_pos_x, m_old_pos_y,
                   m_old_width, m_old_height);
        maximized = MAX_NONE;
    }

}
/**
 * Maximize window horizontal
 */
void FluxboxWindow::maximizeHorizontal() {
    if (! (maximized & MAX_HORZ) ) {
        const int head = screen().getHead(frame().window());
        const unsigned int left_x = screen().maxLeft(head);
        const unsigned int max_width = screen().maxRight(head);
        m_old_width = frame().width();
        m_old_pos_x = frame().x();
        moveResize(left_x, frame().y(), max_width - left_x, frame().height());
        maximized |= MAX_HORZ;
    } else {
        moveResize(m_old_pos_x, frame().y(), m_old_width, frame().height());
        maximized &= ~MAX_HORZ;
    }
}

/**
 * Maximize window vertical
 */
void FluxboxWindow::maximizeVertical() {
    if (! (maximized & MAX_VERT) ) {
        const int head = screen().getHead(frame().window());
        const unsigned int max_top = screen().maxTop(head);
        m_old_height = frame().height();
        m_old_pos_y = frame().y();
        moveResize(frame().x(), max_top, frame().width(), screen().maxBottom(head) - max_top);
        maximized |= MAX_VERT;
    } else {
        moveResize(frame().x(), m_old_pos_y, frame().width(), m_old_height);
        maximized &= ~MAX_VERT;
    }
}


void FluxboxWindow::setWorkspace(int n) {
    unsigned int old_wkspc = m_workspace_number;

    m_workspace_number = n;

    m_blackbox_attrib.flags |= ATTRIB_WORKSPACE;
    m_blackbox_attrib.workspace = m_workspace_number;

    // notify workspace change
    if (!stuck && old_wkspc != m_workspace_number) {
#ifdef DEBUG
        cerr<<this<<" notify workspace signal"<<endl;
#endif // DEBUG
        m_workspacesig.notify();
    }
}

void FluxboxWindow::setLayerNum(int layernum) {
    m_layernum = layernum;

    m_blackbox_attrib.flags |= ATTRIB_STACK;
    m_blackbox_attrib.stack = layernum;
    saveBlackboxAttribs();

#ifdef DEBUG
    cerr<<this<<" notify layer signal"<<endl;
#endif // DEBUG

    m_layersig.notify();
}

void FluxboxWindow::shade() {
    // we can only shade if we have a titlebar
    if (!decorations.titlebar)
        return;

    frame().shade();

    if (shaded) {
        shaded = false;
        m_blackbox_attrib.flags ^= ATTRIB_SHADED;
        m_blackbox_attrib.attrib ^= ATTRIB_SHADED;

        setState(NormalState);
    } else {
        shaded = true;
        m_blackbox_attrib.flags |= ATTRIB_SHADED;
        m_blackbox_attrib.attrib |= ATTRIB_SHADED;
        // shading is the same as iconic
        setState(IconicState);
    }

}


void FluxboxWindow::stick() {

    if (stuck) {
        m_blackbox_attrib.flags ^= ATTRIB_OMNIPRESENT;
        m_blackbox_attrib.attrib ^= ATTRIB_OMNIPRESENT;

        stuck = false;

    } else {
        stuck = true;

        m_blackbox_attrib.flags |= ATTRIB_OMNIPRESENT;
        m_blackbox_attrib.attrib |= ATTRIB_OMNIPRESENT;

    }
 
    setState(m_current_state);
    // notify since some things consider "stuck" to be a pseudo-workspace
    m_workspacesig.notify();

}


void FluxboxWindow::raise() {
    if (isIconic())
        deiconify();

    // get root window
    WinClient *client = getRootTransientFor(m_client);

    // if we don't have any root window use this as root
    if (client == 0) 
        client = m_client;

    // raise this window and every transient in it
    if (client->fbwindow())
        raiseFluxboxWindow(*client->fbwindow());
}

void FluxboxWindow::lower() {
    if (isIconic())
        deiconify();

    // get root window
    WinClient *client = getRootTransientFor(m_client);
    
    // if we don't have any root window use this as root
    if (client == 0) 
        client = m_client;

    if (client->fbwindow())
        lowerFluxboxWindow(*client->fbwindow());
}

void FluxboxWindow::tempRaise() {
    if (isIconic())
        deiconify();

    // get root window
    WinClient *client = getRootTransientFor(m_client);
    
    // if we don't have any root window use this as root
    if (client == 0) 
        client = m_client;

    if (client->fbwindow())
        tempRaiseFluxboxWindow(*client->fbwindow());
}


void FluxboxWindow::raiseLayer() {
    // don't let it up to menu layer
    if (layerNum() == (Fluxbox::instance()->getMenuLayer()+1))
        return;

    // get root window
    WinClient *client = getRootTransientFor(m_client);
    
    // if we don't have any root window use this as root
    if (client == 0) 
        client = m_client;

    FluxboxWindow *win = client->fbwindow();
    if (!win) return;

    if (!win->isIconic())
        screen().updateNetizenWindowRaise(client->window());

    win->layerItem().raiseLayer();

    // remember number just in case a transient happens to revisit this window
    int layer_num = win->layerItem().getLayerNum();
    win->setLayerNum(layer_num);

    WinClient::TransientList::const_iterator it = client->transientList().begin();
    WinClient::TransientList::const_iterator it_end = client->transientList().end();
    for (; it != it_end; ++it) {
        win = (*it)->fbwindow();
        if (win && !win->isIconic()) {
            screen().updateNetizenWindowRaise((*it)->window());
            win->layerItem().moveToLayer(layer_num);
            win->setLayerNum(layer_num);
        }
    }
}

void FluxboxWindow::lowerLayer() {
    // get root window
    WinClient *client = getRootTransientFor(m_client);
    
    // if we don't have any root window use this as root
    if (client == 0) 
        client = m_client;

    FluxboxWindow *win = client->fbwindow();
    if (!win) return;

    if (!win->isIconic()) {
        screen().updateNetizenWindowLower(client->window());
    }
    win->layerItem().lowerLayer();
    // remember number just in case a transient happens to revisit this window
    int layer_num = win->layerItem().getLayerNum();
    win->setLayerNum(layer_num);

    WinClient::TransientList::const_iterator it = client->transientList().begin();
    WinClient::TransientList::const_iterator it_end = client->transientList().end();
    for (; it != it_end; ++it) {
        win = (*it)->fbwindow();
        if (win && !win->isIconic()) {
            screen().updateNetizenWindowLower((*it)->window());
            win->layerItem().moveToLayer(layer_num);
            win->setLayerNum(layer_num);
        }
    }
}


void FluxboxWindow::moveToLayer(int layernum) {
    Fluxbox * fluxbox = Fluxbox::instance();

    // don't let it set its layer into menu area
    if (layernum <= fluxbox->getMenuLayer()) {
        layernum = fluxbox->getMenuLayer() + 1;
    }

    // get root window
    WinClient *client = getRootTransientFor(m_client);
    
    // if we don't have any root window use this as root
    if (client == 0) 
        client = m_client;

    FluxboxWindow *win = client->fbwindow();
    if (!win) return;

    if (!win->isIconic()) {
        screen().updateNetizenWindowRaise(client->window());
    }
    win->layerItem().moveToLayer(layernum);
    // remember number just in case a transient happens to revisit this window
    layernum = win->layerItem().getLayerNum();
    win->setLayerNum(layernum);

    WinClient::TransientList::const_iterator it = client->transientList().begin();
    WinClient::TransientList::const_iterator it_end = client->transientList().end();
    for (; it != it_end; ++it) {
        win = (*it)->fbwindow();
        if (win && !win->isIconic()) {
            screen().updateNetizenWindowRaise((*it)->window());
            win->layerItem().moveToLayer(layernum);
            win->setLayerNum(layernum);
        }
    }
}



void FluxboxWindow::setFocusFlag(bool focus) {
    focused = focus;

    // Record focus timestamp for window cycling enhancements
    if (focused)
        gettimeofday(&m_last_focus_time, 0);

    frame().setFocus(focus);

    if ((screen().isSloppyFocus() || screen().isSemiSloppyFocus()) &&
        screen().doAutoRaise())
        m_timer.stop();
}


void FluxboxWindow::installColormap(bool install) {
    Fluxbox *fluxbox = Fluxbox::instance();
    fluxbox->grab();
    if (! validateClient()) return;

    int i = 0, ncmap = 0;
    Colormap *cmaps = XListInstalledColormaps(display, m_client->window(), &ncmap);
    XWindowAttributes wattrib;
    if (cmaps) { //!!
        if (m_client->getAttrib(wattrib)) {
            if (install) {
                // install the window's colormap
                for (i = 0; i < ncmap; i++) {
                    if (*(cmaps + i) == wattrib.colormap) {
                        // this window is using an installed color map... do not install
                        install = false;
                        break; //end for-loop (we dont need to check more)
                    }
                }
                // otherwise, install the window's colormap
                if (install)
                    XInstallColormap(display, wattrib.colormap);
            } else {				
                for (i = 0; i < ncmap; i++) { // uninstall the window's colormap
                    if (*(cmaps + i) == wattrib.colormap)
                       XUninstallColormap(display, wattrib.colormap);
                }
            }
        }

        XFree(cmaps);
    }

    fluxbox->ungrab();
}

/**
 Saves blackbox attributes for every client in our list
 */
void FluxboxWindow::saveBlackboxAttribs() {
    for_each(m_clientlist.begin(), m_clientlist.end(), 
             FbTk::ChangeProperty(
                 display, 
                 FbAtoms::instance()->getFluxboxAttributesAtom(),
                 PropModeReplace,
                 (unsigned char *)&m_blackbox_attrib,
                 PropBlackboxAttributesElements
                 )
        );
}

/**
 Sets state on each client in our list
 */
void FluxboxWindow::setState(unsigned long new_state) {
    if (numClients() == 0)
        return;

    m_current_state = new_state;
    unsigned long state[2];
    state[0] = (unsigned long) m_current_state;
    state[1] = (unsigned long) None;

    for_each(m_clientlist.begin(), m_clientlist.end(),
             FbTk::ChangeProperty(display, FbAtoms::instance()->getWMStateAtom(),
                            PropModeReplace,
                            (unsigned char *)state, 2));

    saveBlackboxAttribs();
    //notify state changed
    m_statesig.notify();
}

bool FluxboxWindow::getState() {
    m_current_state = 0;

    Atom atom_return;
    bool ret = false;
    int foo;
    unsigned long *state, ulfoo, nitems;
    if ((XGetWindowProperty(display, m_client->window(), FbAtoms::instance()->getWMStateAtom(),
                            0l, 2l, false, FbAtoms::instance()->getWMStateAtom(),
                            &atom_return, &foo, &nitems, &ulfoo,
                            (unsigned char **) &state) != Success) ||
        (! state)) {
        return false;
    }

    if (nitems >= 1) {
        m_current_state = static_cast<unsigned long>(state[0]);
        ret = true;
    }

    XFree(static_cast<void *>(state));

    return ret;
}

//!! TODO: this functions looks odd
void FluxboxWindow::setGravityOffsets() {
    int newx = frame().x();
    int newy = frame().y();
    // translate x coordinate
    switch (m_client->win_gravity) {
        // handle Westward gravity
    case NorthWestGravity:
    case WestGravity:
    case SouthWestGravity:
    default:
#ifdef DEBUG
        cerr<<__FILE__<<": Default gravity: SouthWest, NorthWest, West"<<endl;
#endif // DEBUG

        newx = frame().x();
        break;

        // handle Eastward gravity
    case NorthEastGravity:
    case EastGravity:
    case SouthEastGravity:
#ifdef DEBUG
        cerr<<__FILE__<<": Gravity: SouthEast, NorthEast, East"<<endl;
#endif // DEBUG

        newx = frame().x() + frame().clientArea().width() - frame().width();
        break;

        // no x translation desired - default
    case StaticGravity:
    case ForgetGravity:
    case CenterGravity:
#ifdef DEBUG
        cerr<<__FILE__<<": Gravity: Center, Forget, Static"<<endl;
#endif // DEBUG

        newx = frame().x();
    }

    // translate y coordinate
    switch (m_client->win_gravity) {
        // handle Northbound gravity
    case NorthWestGravity:
    case NorthGravity:
    case NorthEastGravity:
    default:
        newy = frame().y();
        break;

        // handle Southbound gravity
    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
        newy = frame().y() + frame().clientArea().height() - frame().height();
        break;

        // no y translation desired - default
    case StaticGravity:
    case ForgetGravity:
    case CenterGravity:
        newy = frame().y();
        break;
    }
    // finaly move the frame
    if (frame().x() != newx || frame().y() != newy)
        frame().move(newx, newy);
}

/** 
 * Sets the attributes to what they should be
 * but doesn't change the actual state
 * (so the caller can set defaults etc as well)
 */
void FluxboxWindow::restoreAttributes() {
    if (!getState())
        m_current_state = NormalState;

    Atom atom_return;
    int foo;
    unsigned long ulfoo, nitems;
    FbAtoms *fbatoms = FbAtoms::instance();
	
    BlackboxAttributes *net;
    if (m_client->property(fbatoms->getFluxboxAttributesAtom(), 0l,
                           PropBlackboxAttributesElements, false,
                           fbatoms->getFluxboxAttributesAtom(), &atom_return, &foo,
                           &nitems, &ulfoo, (unsigned char **) &net) &&
        net) {
        if (nitems != (unsigned)PropBlackboxAttributesElements) {
            XFree(net);
            return;
        }
        m_blackbox_attrib.flags = net->flags;
        m_blackbox_attrib.attrib = net->attrib;
        m_blackbox_attrib.workspace = net->workspace;
        m_blackbox_attrib.stack = net->stack;
        m_blackbox_attrib.premax_x = net->premax_x;
        m_blackbox_attrib.premax_y = net->premax_y;
        m_blackbox_attrib.premax_w = net->premax_w;
        m_blackbox_attrib.premax_h = net->premax_h;

        XFree(static_cast<void *>(net));
    } else
        return;

    if (m_blackbox_attrib.flags & ATTRIB_SHADED &&
        m_blackbox_attrib.attrib & ATTRIB_SHADED) {
        int save_state =
            ((m_current_state == IconicState) ? NormalState : m_current_state);

        shaded = true;
			
        m_current_state = save_state;
    }

    if (( m_blackbox_attrib.workspace != screen().currentWorkspaceID()) &&
        ( m_blackbox_attrib.workspace < screen().getCount())) {
        m_workspace_number = m_blackbox_attrib.workspace;

        if (m_current_state == NormalState) m_current_state = WithdrawnState;
    } else if (m_current_state == WithdrawnState)
        m_current_state = NormalState;

    if (m_blackbox_attrib.flags & ATTRIB_OMNIPRESENT &&
        m_blackbox_attrib.attrib & ATTRIB_OMNIPRESENT) {
        stuck = true;

        m_current_state = NormalState;
    }

    if (m_blackbox_attrib.flags & ATTRIB_STACK) {
        //!! TODO check value?
        m_layernum = m_blackbox_attrib.stack;
    }

    if ((m_blackbox_attrib.flags & ATTRIB_MAXHORIZ) ||
        (m_blackbox_attrib.flags & ATTRIB_MAXVERT)) {
        int x = m_blackbox_attrib.premax_x, y = m_blackbox_attrib.premax_y;
        unsigned int w = m_blackbox_attrib.premax_w, h = m_blackbox_attrib.premax_h;
        maximized = MAX_NONE;
        if ((m_blackbox_attrib.flags & ATTRIB_MAXHORIZ) &&
            (m_blackbox_attrib.flags & ATTRIB_MAXVERT))
            maximized = MAX_FULL;
        else if (m_blackbox_attrib.flags & ATTRIB_MAXVERT)
            maximizeVertical();
        else if (m_blackbox_attrib.flags & ATTRIB_MAXHORIZ)
            maximizeHorizontal();

        m_blackbox_attrib.premax_x = x;
        m_blackbox_attrib.premax_y = y;
        m_blackbox_attrib.premax_w = w;
        m_blackbox_attrib.premax_h = h;
    }

    setState(m_current_state);
}

/**
   Show the window menu at pos mx, my
*/
void FluxboxWindow::showMenu(int mx, int my) {
    m_windowmenu.move(mx, my);
    m_windowmenu.show();		
    m_windowmenu.raise();
}

/**
   Moves the menu to last button press position and shows it,
   if it's already visible it'll be hidden
 */
void FluxboxWindow::popupMenu() {
    if (m_windowmenu.isVisible()) {
        m_windowmenu.hide(); 
        return;
    }
    // move menu directly under titlebar
    int diff_y = frame().titlebar().height() + frame().titlebar().borderWidth();
    if (!decorations.titlebar) // if we don't have any titlebar
        diff_y = 0;

    m_windowmenu.move(m_last_button_x, frame().y() + diff_y);
    m_windowmenu.show();
    m_windowmenu.raise();
}

void FluxboxWindow::restoreGravity() {
    // restore x coordinate
    switch (m_client->win_gravity) {
        // handle Westward gravity
    case NorthWestGravity:
    case WestGravity:
    case SouthWestGravity:
    default:
        m_client->x = frame().x();
        break;

    // handle Eastward gravity
    case NorthEastGravity:
    case EastGravity:
    case SouthEastGravity:
        m_client->x = (frame().x() + frame().width()) - m_client->width();
        break;
    }

    // restore y coordinate
    switch (m_client->win_gravity) {
        // handle Northbound gravity
    case NorthWestGravity:
    case NorthGravity:
    case NorthEastGravity:
    default:
        m_client->y = frame().y();
        break;

        // handle Southbound gravity
    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
        m_client->y = (frame().y() + frame().height()) - m_client->height();
        break;
    }
}

/**
   Determine if this is the lowest tab of them all
*/
bool FluxboxWindow::isLowerTab() const {
    cerr<<__FILE__<<"("<<__FUNCTION__<<") TODO!"<<endl;
    return true;
}

/**
   Redirect any unhandled event to our handlers
*/
void FluxboxWindow::handleEvent(XEvent &event) {
    switch (event.type) {
    case ConfigureRequest:
        configureRequestEvent(event.xconfigurerequest);
        break;
    case MapNotify:
        mapNotifyEvent(event.xmap);
        break;
        // This is already handled in Fluxbox::handleEvent
        // case MapRequest:
        //        mapRequestEvent(event.xmaprequest);
        //break;
    case PropertyNotify:
        if (event.xproperty.state != PropertyDelete) {
            propertyNotifyEvent(event.xproperty.atom);
        }
        break;

    default:
#ifdef SHAPE
        if (Fluxbox::instance()->haveShape() && 
            event.type == Fluxbox::instance()->shapeEventbase() + ShapeNotify) {
            XShapeEvent *shape_event = (XShapeEvent *)&event;

            if (shape_event->kind != ShapeBounding)
                break;

            if (shape_event->shaped) {
                m_shaped = true;
                shape();
            } else {
                m_shaped = false;
                // set no shape
                Display *disp = FbTk::App::instance()->display();
                XShapeCombineMask(disp,
                                  frame().window().window(), ShapeBounding,
                                  0, 0,
                                  None, ShapeSet);
            }

            XSync(FbTk::App::instance()->display(), False);
            break;
        }
#endif // SHAPE

        break;
    }
}

void FluxboxWindow::mapRequestEvent(XMapRequestEvent &re) {

    // we're only concerned about client window event
    WinClient *client = findClient(re.window);
    if (client == 0) {
#ifdef DEBUG
        cerr<<__FILE__<<"("<<__FUNCTION__<<"): Can't find client!"<<endl;
#endif // DEBUG
        return;
    }

    Fluxbox *fluxbox = Fluxbox::instance();
	
    bool get_state_ret = getState();
    if (! (get_state_ret && fluxbox->isStartup())) {
        if ((m_client->wm_hint_flags & StateHint) &&
            (! (m_current_state == NormalState || m_current_state == IconicState))) {
            m_current_state = m_client->initial_state;
        } else
            m_current_state = NormalState;
    } else if (iconic)
        m_current_state = NormalState;
		
    switch (m_current_state) {
    case IconicState:
        iconify();
	break;

    case WithdrawnState:
        withdraw();
	break;

    case NormalState: {
        // if this window was destroyed while autogrouping
        bool destroyed = false;
       
        // check WM_CLASS only when we changed state to NormalState from 
        // WithdrawnState (ICCC 4.1.2.5)
        client->updateWMClassHint();

        Workspace *wsp = screen().getWorkspace(m_workspace_number);
        if (wsp != 0 && isGroupable())
            destroyed = wsp->checkGrouping(*this);
                
	// if we wasn't grouped with another window we deiconify ourself	
        if (!destroyed)
            deiconify(false);


    } break;
    case InactiveState:
    case ZoomState:
    default:
        deiconify(false);
        break;
    }

}


void FluxboxWindow::mapNotifyEvent(XMapEvent &ne) {
    WinClient *client = findClient(ne.window);
    if (client == 0)
        return;

    if (!ne.override_redirect && isVisible()) {
        Fluxbox *fluxbox = Fluxbox::instance();
        fluxbox->grab();
        if (! validateClient())
            return;

        setState(NormalState);		
			
        if (client->isTransient() || screen().doFocusNew())
            setInputFocus();
        else
            setFocusFlag(false);			

        iconic = false;

        // Auto-group from tab?
        if (!client->isTransient()) {
#ifdef DEBUG
            cerr<<__FILE__<<"("<<__FUNCTION__<<") TODO check grouping here"<<endl;
#endif // DEBUG
        }

        fluxbox->ungrab();
    }
}

/**
   Unmaps frame window and client window if 
   event.window == m_client->window
   Returns true if *this should die
   else false
*/
void FluxboxWindow::unmapNotifyEvent(XUnmapEvent &ue) {
    WinClient *client = findClient(ue.window);
    if (client == 0)
        return;
	
#ifdef DEBUG
    cerr<<__FILE__<<"("<<__FUNCTION__<<"): 0x"<<hex<<client->window()<<dec<<endl;
    cerr<<__FILE__<<"("<<__FUNCTION__<<"): title="<<client->title()<<endl;
#endif // DEBUG
    
    restore(client, false);

}

/**
   Checks if event is for m_client->window.
*/
void FluxboxWindow::destroyNotifyEvent(XDestroyWindowEvent &de) {
    if (de.window == m_client->window()) {
#ifdef DEBUG
        cerr<<__FILE__<<"("<<__LINE__<<"): DestroyNotifyEvent this="<<this<<endl;
#endif // DEBUG
        if (numClients() == 1)
            frame().hide();
    }

}


void FluxboxWindow::propertyNotifyEvent(Atom atom) {

    switch(atom) {
    case XA_WM_CLASS:
    case XA_WM_CLIENT_MACHINE:
    case XA_WM_COMMAND:
        break;

    case XA_WM_TRANSIENT_FOR: {
        // TODO: this property notify should be handled by winclient
        // but for now we'll justhave to update all transient info
        //bool was_transient = isTransient();
        ClientList::iterator it = clientList().begin();
        ClientList::iterator it_end = clientList().end();
        for (; it != it_end; it++) 
            (*it)->updateTransientInfo();
        reconfigure();
        // TODO: this is broken whilst we don't know which client
        // update our layer to be the same layer as our transient for
        //if (isTransient() && isTransient() != was_transient)
        //    layerItem().setLayer(getTransientFor()->layerItem().getLayer());
            
    } break;

    case XA_WM_HINTS:
        m_client->updateWMHints();
        break;

    case XA_WM_ICON_NAME:
        updateIconNameFromClient();
        updateIcon();
        break;

    case XA_WM_NAME:
        updateTitleFromClient();

        if (! iconic)
            screen().getWorkspace(m_workspace_number)->update();
        else
            updateIcon();
		 

        break;

    case XA_WM_NORMAL_HINTS: {
        m_client->updateWMNormalHints();

        if ((m_client->normal_hint_flags & PMinSize) &&
            (m_client->normal_hint_flags & PMaxSize)) {

            if (m_client->max_width != 0 && m_client->max_width <= m_client->min_width &&
                m_client->max_height != 0 && m_client->max_height <= m_client->min_height) {
                decorations.maximize = false;
                decorations.handle = false;
                functions.resize=false;
                functions.maximize=false;
            } else {
                // TODO: is broken while handled by FbW, needs to be in WinClient
                if (! winClient().isTransient()) {
                    decorations.maximize = true;
                    decorations.handle = true;
                    functions.maximize = true;	        
                }
                functions.resize = true;
            }
 
    	}

        // save old values
        int x = frame().x(), y = frame().y();
        unsigned int w = frame().width(), h = frame().height();

        upsize();

        // reconfigure if the old values changed
        if (x != frame().x() || y != frame().y() ||
            w != frame().width() || h != frame().height()) {
            moveResize(x, y, w, h);
        }

        break; 
    }

    default:
        if (atom == FbAtoms::instance()->getWMProtocolsAtom()) {
            getWMProtocols();
            //!!TODO  check this area            
            // reset window actions
            screen().setupWindowActions(*this);
            
        } 
        break;
    }

}


void FluxboxWindow::exposeEvent(XExposeEvent &ee) {
    frame().exposeEvent(ee);
}


void FluxboxWindow::configureRequestEvent(XConfigureRequestEvent &cr) {
    WinClient *client = findClient(cr.window);
    if (client == 0)
        return;

    int cx = frame().x(), cy = frame().y();
    unsigned int cw = frame().width(), ch = frame().height();

    if (cr.value_mask & CWBorderWidth)
        client->old_bw = cr.border_width;

    if (cr.value_mask & CWX)
        cx = cr.x;

    if (cr.value_mask & CWY)
        cy = cr.y;

    if (cr.value_mask & CWWidth)
        cw = cr.width;

    if (cr.value_mask & CWHeight)
        ch = cr.height;

    // whether we should send ConfigureNotify to netizens
    bool send_notify = false;

    // the request is for client window so we resize the frame to it first
    if (frame().width() != cw || frame().height() != ch) {        
        frame().resizeForClient(cw, ch);
        send_notify = true;
    }

    if (frame().x() != cx || frame().y() != cy) {
        move(cx, cy);
        // since we already send a notify in move we don't need to do that again
        send_notify = false;
    } 

    if (cr.value_mask & CWStackMode) {
        switch (cr.detail) {
        case Above:
        case TopIf:
        default:
            raise();
            break;

        case Below:
        case BottomIf:
            lower();
            break;
        }
    }

    sendConfigureNotify(send_notify);
}


void FluxboxWindow::buttonPressEvent(XButtonEvent &be) {
    m_last_button_x = be.x_root;
    m_last_button_y = be.y_root;

    // check frame events first
    frame().buttonPressEvent(be);

    if (be.button == 1 || (be.button == 3 && be.state == Mod1Mask)) {
        if ((! focused) && (! screen().isSloppyFocus())) { //check focus 
            setInputFocus(); 
        }

        if (frame().clientArea() == be.window) {            
            if (screen().clickRaises())
                raise();
            XAllowEvents(display, ReplayPointer, be.time);			
        } else {            
            m_button_grab_x = be.x_root - frame().x() - frame().window().borderWidth();
            m_button_grab_y = be.y_root - frame().y() - frame().window().borderWidth();      
        }
        
        if (m_windowmenu.isVisible())
            m_windowmenu.hide();
    }
}

void FluxboxWindow::buttonReleaseEvent(XButtonEvent &re) {

    if (isMoving())
        stopMoving();		
    else if (isResizing())
        stopResizing();
    else if (m_attaching_tab)
        attachTo(re.x_root, re.y_root);
    else if (re.window == frame().window()) {
        if (re.button == 2 && re.state == Mod1Mask)
            XUngrabPointer(display, CurrentTime);
        else 
            frame().buttonReleaseEvent(re);
    } else {
        frame().buttonReleaseEvent(re);
    }
}


void FluxboxWindow::motionNotifyEvent(XMotionEvent &me) {
    if (isMoving() && me.window == parent()) {
        me.window = frame().window().window();
    }
    bool inside_titlebar = (frame().titlebar() == me.window || frame().label() == me.window ||
                            frame().handle() == me.window || frame().window() == me.window);

    if (Fluxbox::instance()->getIgnoreBorder()
        && !(me.state & Mod1Mask) // really should check for exact matches
        && !(isMoving() || isResizing())) {
        int borderw = frame().window().borderWidth();
        if (me.x_root < (frame().x() + borderw) ||
            me.y_root < (frame().y() + borderw) ||
            me.x_root > (frame().x() + (int)frame().width() + borderw) ||
            me.y_root > (frame().y() + (int)frame().height() + borderw))
            return;
    }

    WinClient *client = 0;
    if (!inside_titlebar) {
        // determine if we're in titlebar
        Client2ButtonMap::iterator it = m_labelbuttons.begin();
        Client2ButtonMap::iterator it_end = m_labelbuttons.end();
        for (; it != it_end; ++it) {
            if ((*it).second->window() == me.window) {
                inside_titlebar = true;
                client = (*it).first;
                break;
            }
        }
    }

    if ((me.state & Button1Mask) && functions.move &&
        inside_titlebar && 
        !isResizing()) {

        if (! isMoving()) {
            startMoving(me.window);
        } else {
            int dx = me.x_root - m_button_grab_x, 
                dy = me.y_root - m_button_grab_y;

            dx -= frame().window().borderWidth();
            dy -= frame().window().borderWidth();

            // Warp to next or previous workspace?, must have moved sideways some
            int moved_x = me.x_root - m_last_resize_x;
            // save last event point
            m_last_resize_x = me.x_root;
            m_last_resize_y = me.y_root;

            if (moved_x && screen().isWorkspaceWarping()) {
                unsigned int cur_id = screen().currentWorkspaceID();
                unsigned int new_id = cur_id;
                const int warpPad = screen().getEdgeSnapThreshold();
                // 1) if we're inside the border threshold
                // 2) if we moved in the right direction
                if (me.x_root >= int(screen().width()) - warpPad - 1 &&
                    moved_x > 0) {
                    //warp right
                    new_id = (cur_id + 1) % screen().getCount();
                    dx = - me.x_root; // move mouse back to x=0
                } else if (me.x_root <= warpPad &&
                           moved_x < 0) {
                    //warp left
                    new_id = (cur_id + screen().getCount() - 1) % screen().getCount();
                    dx = screen().width() - me.x_root-1; // move mouse to screen width - 1
                }
                if (new_id != cur_id) {
                    XWarpPointer(display, None, None, 0, 0, 0, 0, dx, 0);

                    screen().changeWorkspaceID(new_id);

                    m_last_resize_x = me.x_root + dx;
                    
                    // dx is the difference, so our new x is what it would  have been
                    // without the warp, plus the difference.
                    dx += me.x_root - m_button_grab_x;
                }
            }
            // dx = current left side, dy = current top
            doSnapping(dx, dy);
            
            if (! screen().doOpaqueMove()) {
                parent().drawRectangle(screen().rootTheme().opGC(),
                                       m_last_move_x, m_last_move_y, 
                                       frame().width() + 2*frame().window().borderWidth()-1,
                                       frame().height() + 2*frame().window().borderWidth()-1);

                parent().drawRectangle(screen().rootTheme().opGC(),
                                       dx, dy, 
                                       frame().width() + 2*frame().window().borderWidth()-1,
                                       frame().height() + 2*frame().window().borderWidth()-1);
                m_last_move_x = dx;
                m_last_move_y = dy;
            } else {
                moveResize(dx, dy, frame().width(), frame().height());
            }

            if (screen().doShowWindowPos())
                screen().showPosition(dx, dy);
        } // end if moving
    } else if (functions.resize &&
               (((me.state & Button1Mask) && (me.window == frame().gripRight() ||
                                              me.window == frame().gripLeft())) ||
                me.window == frame().window())) {

        bool left = (me.window == frame().gripLeft());

        if (! resizing) {			
            startResizing(me.window, me.x, me.y, left); 
        } else if (resizing) {
            // draw over old rect
            parent().drawRectangle(screen().rootTheme().opGC(),
                                   m_last_resize_x, m_last_resize_y,
                                   m_last_resize_w - 1 + 2 * frame().window().borderWidth(),
                                   m_last_resize_h - 1 + 2 * frame().window().borderWidth());


            // move rectangle
            int gx = 0, gy = 0;

            m_last_resize_h = frame().height() + (me.y - m_button_grab_y);
            if (m_last_resize_h < 1)
                m_last_resize_h = 1;

            if (left) {
                m_last_resize_x = me.x_root - m_button_grab_x;
                if (m_last_resize_x > (signed) (frame().x() + frame().width()))
                    m_last_resize_x = m_last_resize_x + frame().width() - 1;

                left_fixsize(&gx, &gy);
            } else {
                m_last_resize_w = frame().width() + (me.x - m_button_grab_x);
                if (m_last_resize_w < 1) // clamp to 1
                    m_last_resize_w = 1;

                right_fixsize(&gx, &gy);
            }

           // draw resize rectangle
           parent().drawRectangle(screen().rootTheme().opGC(),
                                  m_last_resize_x, m_last_resize_y,
                                  m_last_resize_w - 1 + 2 * frame().window().borderWidth(), 
                                  m_last_resize_h - 1 + 2 * frame().window().borderWidth());

            if (screen().doShowWindowPos())
                screen().showGeometry(gx, gy);
        }
    } else if ((me.state & Button2Mask) && inside_titlebar && client != 0) {

        //
        // drag'n'drop code for tabs
        //
        if (m_attaching_tab == 0) {
            // start drag'n'drop for tab
            m_attaching_tab = client;

            XGrabPointer(display, me.window, False, Button2MotionMask |
                         ButtonReleaseMask, GrabModeAsync, GrabModeAsync,
                         None, frame().theme().moveCursor(), CurrentTime);
            m_last_move_x = me.x_root - 1;
            m_last_move_y = me.y_root - 1;
        
            parent().drawRectangle(screen().rootTheme().opGC(),
                                   m_last_move_x, m_last_move_y,
                                   m_labelbuttons[client]->width(), 
                                   m_labelbuttons[client]->height());
        } else { 
            // we already grabed and started to drag'n'drop tab
            // so we update drag'n'drop-rectangle
            int dx = me.x_root - 1, dy = me.y_root - 1;

            dx -= frame().window().borderWidth();
            dy -= frame().window().borderWidth();

            if (screen().getEdgeSnapThreshold()) {
                int drx = screen().width() - (dx + 1);

                if (dx > 0 && dx < drx && dx < screen().getEdgeSnapThreshold()) 
                    dx = 0;
                else if (drx > 0 && drx < screen().getEdgeSnapThreshold())
                    dx = screen().width() - 1;

                int dty, dby;
		
                dty = dy;
                dby = -dy - 1;

                if (dy > 0 && dty < screen().getEdgeSnapThreshold())
                    dy = 0;
                else if (dby > 0 && dby < screen().getEdgeSnapThreshold())
                    dy = - 1;
		
            }
		
            //erase rectangle
            parent().drawRectangle(screen().rootTheme().opGC(),
                                   m_last_move_x, m_last_move_y, 
                                   m_labelbuttons[client]->width(), 
                                   m_labelbuttons[client]->height());


            // redraw rectangle at new pos
            m_last_move_x = dx;
            m_last_move_y = dy;			
            parent().drawRectangle(screen().rootTheme().opGC(),
                                   m_last_move_x, m_last_move_y,
                                   m_labelbuttons[client]->width(), 
                                   m_labelbuttons[client]->height());

			
        }
    }

}

void FluxboxWindow::enterNotifyEvent(XCrossingEvent &ev) { 

    // ignore grab activates, or if we're not visible
    if (ev.mode == NotifyGrab ||
        !isVisible()) {
        return;
    }

    WinClient *client = 0;
    // don't waste our time scanning if we aren't real sloppy focus
    if (screen().isSloppyFocus()) {
        // determine if we're in a label button (tab)
        Client2ButtonMap::iterator it = m_labelbuttons.begin();
        Client2ButtonMap::iterator it_end = m_labelbuttons.end();
        for (; it != it_end; ++it) {
            if ((*it).second->window() == ev.window) {
                client = (*it).first;
                break;
            }
        }
    }
    if (ev.window == frame().window() ||
        ev.window == m_client->window() || 
        client) {
        if ((screen().isSloppyFocus() || screen().isSemiSloppyFocus())
            && !isFocused() ||
            // or, we are focused, but it isn't the one we want
            client && screen().isSloppyFocus() && (m_client != client)) {
           
            // check that there aren't any subsequent leave notify events in the 
            // X event queue
            XEvent dummy;
            scanargs sa;
            sa.w = ev.window;
            sa.enter = sa.leave = False;
            XCheckIfEvent(display, &dummy, queueScanner, (char *) &sa);   
            
            // if client is set, use setCurrent client, otherwise just setInputFocus
            if ((!sa.leave || sa.inferior) && 
                ((client && setCurrentClient(*client, true)) || setInputFocus()))
                installColormap(True);
        }        
    }
}

void FluxboxWindow::leaveNotifyEvent(XCrossingEvent &ev) { 
    if (ev.window == frame().window())
        installColormap(false);
}

// TODO: functions should not be affected by decoration
void FluxboxWindow::setDecoration(Decoration decoration) {
    switch (decoration) {
    case DECOR_NONE:
        decorations.titlebar = decorations.border = decorations.handle =
            decorations.iconify = decorations.maximize =
            decorations.tab = false; //tab is also a decor
        decorations.menu = true; // menu is present
	//	functions.iconify = functions.maximize = true;
	//	functions.move = true;   // We need to move even without decor
	//	functions.resize = true; // We need to resize even without decor
	break;

    default:
    case DECOR_NORMAL:
        decorations.titlebar = decorations.border = decorations.handle =
            decorations.iconify = decorations.maximize =
            decorations.menu = true;
        functions.resize = functions.move = functions.iconify =
            functions.maximize = true;
	break;

    case DECOR_TINY:
        decorations.titlebar = decorations.iconify = decorations.menu =
            functions.move = functions.iconify = true;
        decorations.border = decorations.handle = decorations.maximize =
            functions.resize = functions.maximize = false;
	break;

    case DECOR_TOOL:
        decorations.titlebar = decorations.menu = functions.move = true;
        decorations.iconify = decorations.border = decorations.handle =
            decorations.maximize = functions.resize = functions.maximize =
            functions.iconify = false;
	break;
    }
    applyDecorations();
    // is this reconfigure necessary???
    reconfigure();

}

// commit current decoration values to actual displayed things
void FluxboxWindow::applyDecorations() {
    frame().clientArea().setBorderWidth(0); // client area bordered by other things

    unsigned int borderW = 0;
    if (decorations.border) 
        borderW = screen().rootTheme().borderWidth();

    if (frame().window().borderWidth() != borderW) {
        frame().window().setBorderWidth(borderW);
        frame().titlebar().setBorderWidth(borderW);
        frame().handle().setBorderWidth(borderW);
        frame().gripLeft().setBorderWidth(borderW);
        frame().gripRight().setBorderWidth(borderW);
        frame().reconfigure();
    }

    // we rely on frame not doing anything if it is already shown/hidden
    if (decorations.titlebar) 
        frame().showTitlebar();
    else
        frame().hideTitlebar();

    if (decorations.handle)
        frame().showHandle();
    else
        frame().hideHandle();

}

void FluxboxWindow::toggleDecoration() {
    //don't toggle decor if the window is shaded
    if (isShaded())
        return;
	
    if (decorations.enabled) { //remove decorations
        setDecoration(DECOR_NONE); 
        decorations.enabled = false;
    } else { //revert back to old decoration
        if (m_old_decoration == DECOR_NONE) { // make sure something happens
            setDecoration(DECOR_NORMAL);
        } else {
            setDecoration(m_old_decoration);
        }
        decorations.enabled = true;
    }
}

void FluxboxWindow::setStrut(Strut *strut) {    
    clearStrut();
    m_strut = strut;
}

void FluxboxWindow::clearStrut() {
    if (m_strut != 0) {
        screen().clearStrut(m_strut);
        m_strut = 0;
    }
}

unsigned int FluxboxWindow::decorationMask() const {
    unsigned int ret = 0;
    if (decorations.titlebar)
        ret |= DECORM_TITLEBAR;
    if (decorations.handle)
        ret |= DECORM_HANDLE;
    if (decorations.border)
        ret |= DECORM_BORDER;
    if (decorations.iconify)
        ret |= DECORM_ICONIFY;
    if (decorations.maximize)
        ret |= DECORM_MAXIMIZE;
    if (decorations.close)
        ret |= DECORM_CLOSE;
    if (decorations.menu)
        ret |= DECORM_MENU;
    if (decorations.sticky)
        ret |= DECORM_STICKY;
    if (decorations.shade)
        ret |= DECORM_SHADE;
    if (decorations.tab)
        ret |= DECORM_TAB;
    if (decorations.enabled)
        ret |= DECORM_ENABLED;
    return ret;
}

void FluxboxWindow::setDecorationMask(unsigned int mask) {
    decorations.titlebar = mask & DECORM_TITLEBAR;
    decorations.handle   = mask & DECORM_HANDLE;
    decorations.border   = mask & DECORM_BORDER;
    decorations.iconify  = mask & DECORM_ICONIFY;
    decorations.maximize = mask & DECORM_MAXIMIZE;
    decorations.close    = mask & DECORM_CLOSE;
    decorations.menu     = mask & DECORM_MENU;
    decorations.sticky   = mask & DECORM_STICKY;
    decorations.shade    = mask & DECORM_SHADE;
    decorations.tab      = mask & DECORM_TAB;
    decorations.enabled  = mask & DECORM_ENABLED;
    applyDecorations();
}

bool FluxboxWindow::validateClient() {
    XSync(display, false);

    XEvent e;
    if (XCheckTypedWindowEvent(display, m_client->window(), DestroyNotify, &e) ||
        XCheckTypedWindowEvent(display, m_client->window(), UnmapNotify, &e)) {
        XPutBackEvent(display, &e);
        Fluxbox::instance()->ungrab();

        return false;
    }

    return true;
}

void FluxboxWindow::startMoving(Window win) {
    moving = true;
    Fluxbox *fluxbox = Fluxbox::instance();
    // grabbing (and masking) on the root window allows us to 
    // freely map and unmap the window we're moving.
    XGrabPointer(display, screen().rootWindow().window(), False, Button1MotionMask |
                 ButtonReleaseMask, GrabModeAsync, GrabModeAsync,
                 screen().rootWindow().window(), frame().theme().moveCursor(), CurrentTime);

    if (m_windowmenu.isVisible())
        m_windowmenu.hide();

    fluxbox->maskWindowEvents(screen().rootWindow().window(), this);

    m_last_move_x = frame().x();
    m_last_move_y = frame().y();
    if (! screen().doOpaqueMove()) {
        fluxbox->grab();
        parent().drawRectangle(screen().rootTheme().opGC(),
                               frame().x(), frame().y(),
                               frame().width() + 2*frame().window().borderWidth()-1, 
                               frame().height() + 2*frame().window().borderWidth()-1);
        screen().showPosition(frame().x(), frame().y());
    }
}

void FluxboxWindow::stopMoving() {
    moving = false;
    Fluxbox *fluxbox = Fluxbox::instance();

    fluxbox->maskWindowEvents(0, 0);

   
    if (! screen().doOpaqueMove()) {
        parent().drawRectangle(screen().rootTheme().opGC(),
                               m_last_move_x, m_last_move_y, 
                               frame().width() + 2*frame().window().borderWidth()-1,
                               frame().height() + 2*frame().window().borderWidth()-1);
        moveResize(m_last_move_x, m_last_move_y, frame().width(), frame().height());
        if (m_workspace_number != screen().currentWorkspaceID()) {
            screen().reassociateWindow(this, screen().currentWorkspaceID(), true);
            frame().show();
        }
        fluxbox->ungrab();
    } else
        moveResize(frame().x(), frame().y(), frame().width(), frame().height());

    screen().hideGeometry();
    XUngrabPointer(display, CurrentTime);
	
    XSync(display, False); //make sure the redraw is made before we continue
}

void FluxboxWindow::pauseMoving() {
    if (screen().doOpaqueMove()) {
        return;
    }

    parent().drawRectangle(screen().rootTheme().opGC(),
                           m_last_move_x, m_last_move_y, 
                           frame().width() + 2*frame().window().borderWidth()-1,
                           frame().height() + 2*frame().window().borderWidth()-1);
    
}


void FluxboxWindow::resumeMoving() {
    if (screen().doOpaqueMove()) {
        return;
    }
    
    if (m_workspace_number == screen().currentWorkspaceID()) {
        frame().show();
    }
    XSync(display,false);

    parent().drawRectangle(screen().rootTheme().opGC(),
                           m_last_move_x, m_last_move_y, 
                           frame().width() + 2*frame().window().borderWidth()-1,
                           frame().height() + 2*frame().window().borderWidth()-1);

}

/**
 * Helper function that snaps a window to another window
 * We snap if we're closer than the x/ylimits.
 */
inline void snapToWindow(int &xlimit, int &ylimit, 
                         int left, int right, int top, int bottom,
                         int oleft, int oright, int otop, int obottom) {
    // Only snap if we're adjacent to the edge we're looking at

    // for left + right, need to be in the right y range
    if (top <= obottom && bottom >= otop) {
        // left
        if (abs(left-oleft)  < abs(xlimit)) xlimit = -(left-oleft);
        if (abs(right-oleft) < abs(xlimit)) xlimit = -(right-oleft);
        
        // right
        if (abs(left-oright)  < abs(xlimit)) xlimit = -(left-oright);
        if (abs(right-oright) < abs(xlimit)) xlimit = -(right-oright);
    }
    
    // for top + bottom, need to be in the right x range
    if (left <= oright && right >= oleft) {
        // top
        if (abs(top-otop)    < abs(ylimit)) ylimit = -(top-otop);
        if (abs(bottom-otop) < abs(ylimit)) ylimit = -(bottom-otop);
        
        // bottom
        if (abs(top-obottom)    < abs(ylimit)) ylimit = -(top-obottom);
        if (abs(bottom-obottom) < abs(ylimit)) ylimit = -(bottom-obottom);
    }
    
}

/*
 * Do Whatever snapping magic is necessary, and return using the orig_left 
 * and orig_top variables to indicate the new x,y position
 */
void FluxboxWindow::doSnapping(int &orig_left, int &orig_top) {
    /*
     * Snap to screen/head edges
     * Snap to windows
     */

    if (screen().getEdgeSnapThreshold() == 0) return;

    // Keep track of our best offsets so far
    // We need to find things less than or equal to the threshold
    int dx = screen().getEdgeSnapThreshold() + 1;
    int dy = screen().getEdgeSnapThreshold() + 1;

    // we only care about the left/top etc that includes borders
    int borderW = frame().window().borderWidth();

    int top = orig_top; // orig include the borders
    int left = orig_left;
    int right = orig_left + width() + 2*borderW;
    int bottom = orig_top + height() + 2*borderW;

    /////////////////////////////////////
    // begin by checking the screen (or Xinerama head) edges

    if (screen().hasXinerama()) {
        // head "0" == whole screen width + height, which we skip since the
        // sum of all the heads covers those edges
        for (int h = 1; h <= screen().numHeads(); h++) {
            snapToWindow(dx, dy, left, right, top, bottom, 
                         screen().maxLeft(h),
                         screen().maxRight(h),
                         screen().maxTop(h),
                         screen().maxBottom(h));
        }
    } else
        snapToWindow(dx, dy, left, right, top, bottom, 0, screen().width(), 0, screen().height());
    
    /////////////////////////////////////
    // now check window edges

    Workspace::Windows &wins = 
        screen().currentWorkspace()->windowList();

    Workspace::Windows::iterator it = wins.begin();
    Workspace::Windows::iterator it_end = wins.end();

    for (; it != it_end; it++) {
        if ((*it) == this) continue; // skip myself

        snapToWindow(dx, dy, left, right, top, bottom, 
                     (*it)->x(),
                     (*it)->x() + (*it)->width()  + 2*borderW,
                     (*it)->y(),
                     (*it)->y() + (*it)->height() + 2*borderW);
    }

    // commit
    if (dx <= screen().getEdgeSnapThreshold()) 
        orig_left += dx;
    if (dy <= screen().getEdgeSnapThreshold()) 
        orig_top  += dy;

}


void FluxboxWindow::startResizing(Window win, int x, int y, bool left) {
    resizing = true;

    XGrabPointer(display, win, false, ButtonMotionMask | ButtonReleaseMask, 
                 GrabModeAsync, GrabModeAsync, None,
                 (left ? frame().theme().lowerLeftAngleCursor() : frame().theme().lowerRightAngleCursor()),
                 CurrentTime);

    int gx = 0, gy = 0;
    m_button_grab_x = x;
    m_button_grab_y = y;
    m_last_resize_x = frame().x();
    m_last_resize_y = frame().y();
    m_last_resize_w = frame().width();
    m_last_resize_h = frame().height();

    if (left)
        left_fixsize(&gx, &gy);
    else
        right_fixsize(&gx, &gy);

    if (screen().doShowWindowPos())
        screen().showGeometry(gx, gy);

    parent().drawRectangle(screen().rootTheme().opGC(),
                           m_last_resize_x, m_last_resize_y,
                           m_last_resize_w - 1 + 2 * frame().window().borderWidth(),
                           m_last_resize_h - 1 + 2 * frame().window().borderWidth());
}

void FluxboxWindow::stopResizing(Window win) {
    resizing = false;
	
    parent().drawRectangle(screen().rootTheme().opGC(),
                           m_last_resize_x, m_last_resize_y,
                           m_last_resize_w - 1 + 2 * frame().window().borderWidth(),
                           m_last_resize_h - 1 + 2 * frame().window().borderWidth());

    screen().hideGeometry();

    if (win && win == frame().gripLeft())
        left_fixsize();
    else
        right_fixsize();

	
    moveResize(m_last_resize_x, m_last_resize_y,
               m_last_resize_w, m_last_resize_h);
	
    XUngrabPointer(display, CurrentTime);
}

void FluxboxWindow::attachTo(int x, int y) {
    if (m_attaching_tab == 0)
        return;

    XUngrabPointer(display, CurrentTime);


    parent().drawRectangle(screen().rootTheme().opGC(),
                           m_last_move_x, m_last_move_y, 
                           m_labelbuttons[m_attaching_tab]->width(), 
                           m_labelbuttons[m_attaching_tab]->height());
            
    int dest_x = 0, dest_y = 0;
    Window child = 0;

    if (XTranslateCoordinates(display, parent().window(), 
                              parent().window(),
                              x, y, &dest_x, &dest_y, &child)) {        
        // search for a fluxboxwindow 
        FluxboxWindow *attach_to_win = Fluxbox::instance()->searchWindow(child);

        if (attach_to_win != this &&
            attach_to_win != 0) {
            attach_to_win->attachClient(*m_attaching_tab);
        } else if (attach_to_win != this) { // disconnect client if we didn't drop on a window
            detachClient(*m_attaching_tab);
        }
                    
    }
    m_attaching_tab = 0;
}

void FluxboxWindow::updateIcon() {
    //!! TODO: notify listeners about icon name change
}

void FluxboxWindow::restore(WinClient *client, bool remap) {
    if (client->m_win != this)
        return;

    XChangeSaveSet(display, client->window(), SetModeDelete);
    client->setEventMask(NoEventMask);

    //!! TODO
    //restoreGravity();

    client->hide();

    // restore old border width
    client->setBorderWidth(client->old_bw);

    XEvent not_used;
    if (! XCheckTypedWindowEvent(display, client->window(), ReparentNotify,
                                 &not_used)) {
#ifdef DEBUG
        cerr<<"FluxboxWindow::restore: reparent 0x"<<hex<<client->window()<<dec<<" to root"<<endl;
#endif // DEBUG

        // reparent to root window
        client->reparent(screen().rootWindow().window(), frame().x(), frame().y());        
    }

    if (remap)
        client->show();

    delete client;

#ifdef DEBUG
        cerr<<__FILE__<<"("<<__FUNCTION__<<"): numClients() = "<<numClients()<<endl;
#endif // DEBUG
    if (numClients() == 0) {

        frame().hide();
    }

}

void FluxboxWindow::restore(bool remap) {
    if (numClients() == 0)
        return;

    while (!clientList().empty()) {
        restore(clientList().back(), remap);
        // deleting winClient removes it from the clientList
    }
}

void FluxboxWindow::timeout() {
    raise();
}

bool FluxboxWindow::isVisible() const { 
    return frame().isVisible(); 
}

FbTk::FbWindow &FluxboxWindow::fbWindow() { 
    return frame().window(); 
}

const FbTk::FbWindow &FluxboxWindow::fbWindow() const { 
    return frame().window(); 
}

int FluxboxWindow::x() const {
    return frame().x();
}

int FluxboxWindow::y() const {
    return frame().y();
}

unsigned int FluxboxWindow::width() const { 
    return frame().width();
}

unsigned int FluxboxWindow::height() const { 
    return frame().height();
}

unsigned int FluxboxWindow::titlebarHeight() const {
    return frame().titlebarHeight();
}

Window FluxboxWindow::clientWindow() const  { 
    if (m_client == 0)
        return 0;
    return m_client->window(); 
}

const std::string &FluxboxWindow::title() const { 
    static string empty_string("");
    if (m_client == 0)
        return empty_string;
    return m_client->title();
}

const std::string &FluxboxWindow::iconTitle() const { 
    static string empty_string("");
    if (m_client == 0)
        return empty_string;
    return m_client->iconTitle();
}

int FluxboxWindow::initialState() const { return m_client->initial_state; }

void FluxboxWindow::changeBlackboxHints(const BlackboxHints &net) {
    if ((net.flags & ATTRIB_SHADED) &&
        ((m_blackbox_attrib.attrib & ATTRIB_SHADED) !=
         (net.attrib & ATTRIB_SHADED)))
        shade();

    if ((net.flags & (ATTRIB_MAXVERT | ATTRIB_MAXHORIZ)) &&
        ((m_blackbox_attrib.attrib & (ATTRIB_MAXVERT | ATTRIB_MAXHORIZ)) !=
         (net.attrib & (ATTRIB_MAXVERT | ATTRIB_MAXHORIZ)))) {
        if (maximized) {
            maximize();
        } else {
            if ((net.flags & ATTRIB_MAXHORIZ) && (net.flags & ATTRIB_MAXVERT))
            	maximize();
            else if (net.flags & ATTRIB_MAXVERT)
                maximizeVertical();
            else if (net.flags & ATTRIB_MAXHORIZ)
                maximizeHorizontal();

        }
    }
    
    if ((net.flags & ATTRIB_OMNIPRESENT) &&
        ((m_blackbox_attrib.attrib & ATTRIB_OMNIPRESENT) !=
         (net.attrib & ATTRIB_OMNIPRESENT)))
        stick();

    if ((net.flags & ATTRIB_WORKSPACE) &&
        (m_workspace_number !=  net.workspace)) {

        screen().reassociateWindow(this, net.workspace, true);

        if (screen().currentWorkspaceID() != net.workspace)
            withdraw();
        else 
            deiconify();
    }

    if (net.flags & ATTRIB_STACK) {
        if ((unsigned int) m_layernum != net.stack) {
            moveToLayer(net.stack);
        }
    }

    if (net.flags & ATTRIB_DECORATION) {
        m_old_decoration = static_cast<Decoration>(net.decoration);
        setDecoration(m_old_decoration);
    }

}

void FluxboxWindow::upsize() {
    frame().setBevel(screen().rootTheme().bevelWidth());
    frame().handle().resize(frame().handle().width(), 
                            screen().rootTheme().handleWidth());
    frame().gripLeft().resize(frame().buttonHeight(), 
                              screen().rootTheme().handleWidth());
    frame().gripRight().resize(frame().gripLeft().width(), 
                               frame().gripLeft().height());
}


///TODO
void FluxboxWindow::downsize() {
	
}


void FluxboxWindow::right_fixsize(int *gx, int *gy) {
    // calculate the size of the client window and conform it to the
    // size specified by the size hints of the client window...
    int dx = m_last_resize_w - m_client->base_width;
    int titlebar_height = (decorations.titlebar ? 
                           frame().titlebar().height() +
                           frame().titlebar().borderWidth() : 0);
    int handle_height = (decorations.handle ? 
                         frame().handle().height() +
                         frame().handle().borderWidth() : 0);

    int dy = m_last_resize_h - m_client->base_height - titlebar_height - handle_height;
    if (dx < (signed) m_client->min_width)
        dx = m_client->min_width;
    if (dy < (signed) m_client->min_height)
        dy = m_client->min_height;
    if (m_client->max_width > 0 && (unsigned) dx > m_client->max_width)
        dx = m_client->max_width;
    if (m_client->max_height > 0 && (unsigned) dy > m_client->max_height)
        dy = m_client->max_height;

    // make it snap

    if (m_client->width_inc == 0)
        m_client->width_inc = 1;
    if (m_client->height_inc == 0)
        m_client->height_inc = 1;

    dx /= m_client->width_inc;
    dy /= m_client->height_inc;

    if (gx) *gx = dx;
    if (gy) *gy = dy;

    dx = (dx * m_client->width_inc) + m_client->base_width;
    dy = (dy * m_client->height_inc) + m_client->base_height + 
        titlebar_height + handle_height;

    m_last_resize_w = dx;
    m_last_resize_h = dy;
}

void FluxboxWindow::left_fixsize(int *gx, int *gy) {   
    int titlebar_height = (decorations.titlebar ? 
                           frame().titlebar().height()  + 
                           frame().titlebar().borderWidth() : 0);
    int handle_height = (decorations.handle ? 
                         frame().handle().height() + 
                         frame().handle().borderWidth() : 0);
    int decoration_height = titlebar_height + handle_height;

    // dx is new width = current width + difference between new and old x values
    int dx = frame().width() + frame().x() - m_last_resize_x;

    // dy = new height (w/o decorations), similarly
    int dy = m_last_resize_h - m_client->base_height - decoration_height;

    // check minimum size
    if (dx < static_cast<signed int>(m_client->min_width))
        dx = m_client->min_width;
    if (dy < static_cast<signed int>(m_client->min_height))
        dy = m_client->min_height;

    // check maximum size
    if (m_client->max_width > 0 && dx > static_cast<signed int>(m_client->max_width))
        dx = m_client->max_width;
    if (m_client->max_height > 0 && dy > static_cast<signed int>(m_client->max_height))
        dy = m_client->max_height;

    // make sure we have valid increment
    if (m_client->width_inc == 0)
        m_client->width_inc = 1;
    if (m_client->height_inc == 0)
        m_client->height_inc = 1;

    // set snapping
    dx /= m_client->width_inc;
    dy /= m_client->height_inc;

    // set return values
    if (gx != 0)
        *gx = dx;
    if (gy != 0)
        *gy = dy;

    // snapping
    dx = dx * m_client->width_inc + m_client->base_width;
    dy = dy * m_client->height_inc + m_client->base_height + decoration_height;

    // update last resize 
    m_last_resize_w = dx;
    m_last_resize_h = dy;
    m_last_resize_x = frame().x() + frame().width() - m_last_resize_w;	
}

void FluxboxWindow::resizeClient(WinClient &client, 
                                 unsigned int height, unsigned int width) {
    client.resize(frame().clientArea().width(),
                  frame().clientArea().height());
    client.updateRect(frame().x() + frame().clientArea().x(),
                      frame().y() + frame().clientArea().y(),
                      frame().clientArea().width(),
                      frame().clientArea().height());    
}

void FluxboxWindow::sendConfigureNotify(bool send_to_netizens) {
    ClientList::iterator client_it = m_clientlist.begin();
    ClientList::iterator client_it_end = m_clientlist.end();
    for (; client_it != client_it_end; ++client_it) {
        WinClient &client = *(*client_it);
        /*
          Send event telling where the root position 
          of the client window is. (ie frame pos + client pos inside the frame = send pos)
        */
        //!!
        client.x = frame().x();
        client.y = frame().y();
        resizeClient(client, 
                     frame().clientArea().width(),
                     frame().clientArea().height());

        
        XEvent event;
        event.type = ConfigureNotify;

        event.xconfigure.display = display;
        event.xconfigure.event = client.window();
        event.xconfigure.window = client.window();
        event.xconfigure.x = frame().x() + frame().clientArea().x();
        event.xconfigure.y = frame().y() + frame().clientArea().y();
        event.xconfigure.width = client.width();
        event.xconfigure.height = client.height();
        event.xconfigure.border_width = client.old_bw;
        event.xconfigure.above = frame().window().window();
        event.xconfigure.override_redirect = false;

        XSendEvent(display, client.window(), False, StructureNotifyMask, &event);

        if (send_to_netizens)
            screen().updateNetizenConfigNotify(event);
    } // end for        
}


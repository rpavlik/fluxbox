// FbRun.cc
// Copyright (c) 2002-2003 Henrik Kinnunen (fluxgen<at>users.sourceforge.net)
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
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

// $Id: FbRun.cc,v 1.20 2003/09/06 15:50:25 fluxgen Exp $

#include "FbRun.hh"

#include "App.hh"
#include "EventManager.hh"
#include "Color.hh"
#include "KeyUtil.hh"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // HAVE_CONFIG_H

#ifdef HAVE_XPM
#include <X11/xpm.h>
#include "fbrun.xpm"
#endif // HAVE_XPM

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>

#include <unistd.h>

#include <iostream>
#include <iterator>
#include <fstream>
#include <cassert>

using namespace std;
FbRun::FbRun(int x, int y, size_t width):
    FbTk::TextBox(DefaultScreen(FbTk::App::instance()->display()),
                  m_font, ""),
    m_font("fixed"),
    m_display(FbTk::App::instance()->display()),
    m_bevel(4),
    m_gc(*this),
    m_end(false),
    m_current_history_item(0),
    m_cursor(XCreateFontCursor(FbTk::App::instance()->display(), XC_xterm)) {
    
    setGC(m_gc.gc());
    setCursor(m_cursor);
    // setting nomaximize in local resize
    resize(width, font().height() + m_bevel);

    // setup class name
    XClassHint *class_hint = XAllocClassHint();
    if (class_hint == 0)
        throw string("Out of memory");
    class_hint->res_name = "fbrun";
    class_hint->res_class = "FbRun";    
    XSetClassHint(m_display, window(), class_hint);
    
    XFree(class_hint);
#ifdef HAVE_XPM
    Pixmap mask = 0;
    Pixmap pm;
    XpmCreatePixmapFromData(m_display,
                            window(),
                            fbrun_xpm,
                            &pm,
                            &mask,
                            0); // attribs
    if (mask != 0)
        XFreePixmap(m_display, mask);

    m_pixmap = pm;
#endif // HAVE_XPM

    if (m_pixmap.drawable()) {
        XWMHints wmhints;
        wmhints.flags = IconPixmapHint;
        wmhints.icon_pixmap = m_pixmap.drawable();
        XSetWMHints(m_display, window(), &wmhints);
    }
}


FbRun::~FbRun() {
    hide();
}

void FbRun::run(const std::string &command) {
    FbTk::App::instance()->end(); // end application
    m_end = true; // mark end of processing

    // fork and execute program
    if (!fork()) {
        setsid();
        execl("/bin/sh", "/bin/sh", "-c", command.c_str(), 0);
        exit(0); //exit child
    }

    hide(); // hide gui
    
    // save command history to file
    if (text().size() != 0) { // no need to save empty command

        // don't allow duplicates into the history file, first
        // look for a duplicate
        if (m_current_history_item < m_history.size()
            && text() == m_history[m_current_history_item]) {
            // m_current_history_item is the duplicate
        } else {
            m_current_history_item = 0;
            for (; m_current_history_item < m_history.size(); 
                 ++m_current_history_item) {
                if (m_history[m_current_history_item] == text())
                    break;
            }
        }

        // now m_current_history_item points at the duplicate, or
        // at m_history.size() if no duplicate
        fstream inoutfile(m_history_file.c_str(), ios::in|ios::out);
        if (inoutfile) {
            int i = 0;
            // read past history items before current
            for (string line; !inoutfile.eof() && i < m_current_history_item; i++)
                getline(inoutfile, line);
            // write the history items that come after current
            for (i++; i < m_history.size(); i++)
                inoutfile<<m_history[i]<<endl;
            
            // and append the current one back to the end
            inoutfile<<text()<<endl;
        } else
            cerr<<"FbRun Warning: Can't write command history to file: "<<m_history_file<<endl;
    }

}

bool FbRun::loadHistory(const char *filename) {
    if (filename == 0)
        return false;
    ifstream infile(filename);
    if (!infile) {
        //even though we fail to load file, we should try save to it
        m_history_file = filename;
        return false;
    }
    // clear old history and load new one from file
    m_history.clear();
    // each line is a command
    string line;
    while (!infile.eof()) {
        getline(infile, line);
        if (line.size()) // don't add empty lines
            m_history.push_back(line);
    }
    // set no current histor to display
    m_current_history_item = m_history.size();
    // set history file
    m_history_file = filename;
    return true;
}

bool FbRun::loadFont(const string &fontname) {
    if (!m_font.load(fontname.c_str()))
        return false;

    // resize to fit new font height
    resize(width(), font().height() + m_bevel);
    return true;
}

void FbRun::setForegroundColor(const FbTk::Color &color) {
    m_gc.setForeground(color);
}

void FbRun::setTitle(const string &title) {
    setName(title.c_str());
}

void FbRun::resize(size_t width, size_t height) {
    FbTk::TextBox::resize(width, height);    
    setNoMaximize();
}

void FbRun::redrawLabel() {
    clear();
}

void FbRun::keyPressEvent(XKeyEvent &ke) {
    // strip numlock and scrolllock mask
    ke.state &= ~FbTk::KeyUtil::numlockMod();
    ke.state &= ~FbTk::KeyUtil::scrolllockMod();

    FbTk::TextBox::keyPressEvent(ke);
    KeySym ks;
    char keychar[1];
    XLookupString(&ke, keychar, 1, &ks, 0);
    // a modifier key by itself doesn't do anything
    if (IsModifierKey(ks)) return;

    if (ke.state) { // a modifier key is down
        if (ke.state == ControlMask) {
            switch (ks) {
            case XK_p:
                prevHistoryItem();
                break;
            case XK_n:
                nextHistoryItem();
                break;
            }
        } else if (ke.state == (Mod1Mask | ShiftMask)) {
            switch (ks) {
            case XK_less:
                firstHistoryItem();
                break;
            case XK_greater:
                lastHistoryItem();
                break;
            }
        }
    } else { // no modifier key
        switch (ks) {
        case XK_Escape:
            m_end = true;
            hide();
            FbTk::App::instance()->end(); // end program
            break;
        case XK_Return:
            run(text());
            break;
        case XK_Up:
            prevHistoryItem();
            break;
        case XK_Down:
            nextHistoryItem();
            break;
        case XK_Tab:
            tabCompleteHistory();
            break;
        }
    }
    clear();
}

void FbRun::setNoMaximize() {
    // we don't need to maximize this window
    XSizeHints sh;
    sh.flags = PMaxSize | PMinSize;
    sh.max_width = width();
    sh.max_height = height();
    sh.min_width = width();
    sh.min_height = height();
    XSetWMNormalHints(m_display, window(), &sh);
}

void FbRun::prevHistoryItem() {
    if (m_history.size() == 0 || m_current_history_item == 0) {
        XBell(m_display, 0);
    } else {
        m_current_history_item--;
        setText(m_history[m_current_history_item]);
    }
}

void FbRun::nextHistoryItem() {
    if (m_current_history_item == m_history.size()) {
        XBell(m_display, 0);
    } else {
        m_current_history_item++;
        if (m_current_history_item == m_history.size()) {
            m_current_history_item = m_history.size();
            setText("");
         } else
            setText(m_history[m_current_history_item]);
    }
}

void FbRun::firstHistoryItem() {
    if (m_history.size() == 0 || m_current_history_item == 0) {
        XBell(m_display, 0);
    } else {
        m_current_history_item = 0;
        setText(m_history[m_current_history_item]);
    }
}

void FbRun::lastHistoryItem() {
    // actually one past the end
    if (m_history.size() == 0) {
        XBell(m_display, 0);
    } else {
        m_current_history_item = m_history.size();
        setText("");
    }
}

void FbRun::tabCompleteHistory() {
    if (m_current_history_item == 0) {
        XBell(m_display, 0);
    } else {
        int history_item = m_current_history_item - 1;
        string prefix = text().substr(0, cursorPosition());
        while (history_item > - 1) {
            if (m_history[history_item].find(prefix) == 0) {
                m_current_history_item = history_item;
                setText(m_history[m_current_history_item]);
                break;
            }
            history_item--;
        }
        if (history_item == -1) XBell(m_display, 0);
    }
}

void FbRun::insertCharacter(char keychar) {
    char val[2] = {keychar, 0};
    insertText(val);
}


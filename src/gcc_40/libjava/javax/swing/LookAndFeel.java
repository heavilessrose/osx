/* LookAndFeel.java --
   Copyright (C) 2002, 2004  Free Software Foundation, Inc.

This file is part of GNU Classpath.

GNU Classpath is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU Classpath is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Classpath; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
02111-1307 USA.

Linking this library statically or dynamically with other modules is
making a combined work based on this library.  Thus, the terms and
conditions of the GNU General Public License cover the whole
combination.

As a special exception, the copyright holders of this library give you
permission to link this library with independent modules to produce an
executable, regardless of the license terms of these independent
modules, and to copy and distribute the resulting executable under
terms of your choice, provided that you also meet, for each linked
independent module, the terms and conditions of the license of that
module.  An independent module is a module which is not derived from
or based on this library.  If you modify this library, you may extend
this exception to your version of the library, but you are not
obligated to do so.  If you do not wish to do so, delete this
exception statement from your version. */

package javax.swing;

import java.awt.Component;
import java.awt.Toolkit;
import javax.swing.text.JTextComponent;


public abstract class LookAndFeel
{
  /**
   * This method is called once by UIManager.setLookAndFeel to create
   * the look and feel specific defaults table.
   *
   * @return the UI defaults
   */
  public UIDefaults getDefaults()
  {
    return null;
  }

  public abstract String getDescription();

  public abstract String getID();

  public abstract String getName();

  /**
   * Returns true when the Look and Feel supports window decorations,
   * false others. This method returns always false and needs to be overwritten
   * when the derived Look and Feel supports this.
   *
   * @return false
   *
   * @since 1.4
   */
  public boolean getSupportsWindowDecorations()
  {
    return false;
  }
  
  /**
   * UIManager.setLookAndFeel calls this method before the first call
   * (and typically the only call) to getDefaults(). 
   */
  public void initialize()
  {
  }

  /**
   * Convenience method for installing a component's default Border object
   * on the specified component if either the border is currently null
   * or already an instance of UIResource. 
   */
  public static void installBorder(JComponent c, String defaultBorderName)
  {
  }

  /**
   * Convenience method for initializing a component's foreground and
   * background color properties with values from the current defaults table.
   */
  public static void installColors(JComponent c, String defaultBgName, String defaultFgName)
  {
  }

  /**
   * Convenience method for initializing a components foreground background
   * and font properties with values from the current defaults table. 
   */
  public static void installColorsAndFont(JComponent component,
					  String defaultBgName,
					  String defaultFgName,
					  String defaultFontName)
  {
  }

  public abstract boolean isNativeLookAndFeel();

  public abstract boolean isSupportedLookAndFeel();

  /**
   * Loads the bindings in keys into retMap. 
   */
  public static void loadKeyBindings(InputMap retMap, Object[] keys)
  {
  }

  /**
   * Creates a ComponentInputMap from keys. 
   */
  public static ComponentInputMap makeComponentInputMap(JComponent c,
							Object[] keys)
  {
    return null;
  }

  /**
   * Utility method that creates a UIDefaults.LazyValue that creates an
   * ImageIcon UIResource for the specified gifFile filename. 
   */
  public static Object makeIcon(Class baseClass, String gifFile)
  {
    return null;
  }

  /**
   * Creates a InputMap from keys. 
   */
  public static InputMap makeInputMap(Object[] keys)
  {
    return null;
  }

  /**
   * Convenience method for building lists of KeyBindings.  
   */
  public static JTextComponent.KeyBinding[] makeKeyBindings(Object[] keyBindingList)
  {
    return null;
  }

  /**
   * Invoked when the user attempts an invalid operation. The default implement
   * just beeps. Subclasses that wish to change this need to override this
   * method.
   *
   * @param component the component the error occured in
   */
  public void provideErrorFeedback(Component component)
  {
    Toolkit.getDefaultToolkit().beep();
  }

  /**
   * Returns a string that displays and identifies this object's properties.
   *
   * @return the string "LookAndFeel"
   */
  public String toString()
  {
    return "LookAndFeel";
  }

  /**
   * UIManager.setLookAndFeel calls this method just before we're replaced by
   * a new default look and feel. 
   */
  public void uninitialize()
  {
  }

  /**
   * Convenience method for un-installing a component's default border on the
   * specified component if the border is currently an instance of UIResource.
   */
  public static void uninstallBorder(JComponent c)
  {
  }
}

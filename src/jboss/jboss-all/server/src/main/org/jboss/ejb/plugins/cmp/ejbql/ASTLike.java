/*
 * JBoss, the OpenSource J2EE webOS
 *
 * Distributable under LGPL license.
 * See terms of license at gnu.org.
 */
 
package org.jboss.ejb.plugins.cmp.ejbql;

import org.jboss.ejb.plugins.cmp.jdbc.SQLUtil;

/**
 * This abstract syntax node represents a like opperator.
 *
 * @author <a href="mailto:dain@daingroup.com">Dain Sundstrom</a>
 * @version $Revision: 1.1.4.2 $
 */                            
public final class ASTLike extends SimpleNode {
   public boolean not;

   public ASTLike(int id) {
      super(id);
   }

   public String toString() {
      return (not ? SQLUtil.NOT : SQLUtil.EMPTY_STRING) + SQLUtil.LIKE;
   }

   /** Accept the visitor. **/
   public Object jjtAccept(JBossQLParserVisitor visitor, Object data) {
      return visitor.visit(this, data);
   }
}

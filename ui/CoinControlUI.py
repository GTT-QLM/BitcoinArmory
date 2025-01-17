from __future__ import (absolute_import, division,
                        print_function, unicode_literals)
 ##############################################################################
#                                                                            #
# Copyright (C) 2016-17, goatpig                                             #
#  Distributed under the MIT license                                         #
#  See LICENSE-MIT or https://opensource.org/licenses/MIT                    #
#                                                                            #
##############################################################################

from armoryengine.Settings import TheSettings
from qtdialogs.qtdefines import QRichLabel, makeHorizFrame, \
   saveTableView, restoreTableView, createToolTipWidget
from qtdialogs.ArmoryDialog import ArmoryDialog

from ui.TreeViewGUI import CoinControlTreeModel, RBFTreeModel

from PySide2.QtCore import QByteArray
from PySide2.QtWidgets import QCheckBox, QTreeView, QPushButton, \
   QDialogButtonBox, QGridLayout

################################################################################
class CoinControlDlg(ArmoryDialog):

   #############################################################################
   def __init__(self, parent, main, wlt):
      super(CoinControlDlg, self).__init__(parent, main)

      self.wlt = wlt

      lblDescr = QRichLabel(self.tr(
         'By default, transactions are created using any available coins from '
         'all addresses in this wallet.  You can control the source addresses '
         'used for this transaction by selecting them below, and unchecking '
         'all other addresses.'))

      self.useAllCheckBox = QCheckBox(self.tr("Use all selected UTXOs"))
      useAllToolTip = createToolTipWidget(self.tr(
      'By default, Armory will pick a subset of the UTXOs you chose '
      'explicitly through the coin control feature to best suit the '
      'total spend value of the transaction you are constructing. '
      '<br><br>'
      'Checking \'Use all selected UTXOs\' forces the construction of a '
      'transaction that will redeem the exact list of UTXOs you picked '
      'instead.'))

      frmCheckAll = makeHorizFrame([self.useAllCheckBox, useAllToolTip, 'Stretch'])

      self.ccTreeModel = CoinControlTreeModel(self, wlt)
      self.ccView = QTreeView()
      self.ccView.setModel(self.ccTreeModel)

      self.setMinimumWidth(400)
      self.setMinimumHeight(300)

      self.btnAccept = QPushButton(self.tr("Accept"))
      self.btnCancel = QPushButton(self.tr("Cancel"))
      self.btnAccept.clicked.connect(self.accept)
      self.btnCancel.clicked.connect(self.reject)
      buttonBox = QDialogButtonBox()
      buttonBox.addButton(self.btnAccept, QDialogButtonBox.AcceptRole)
      buttonBox.addButton(self.btnCancel, QDialogButtonBox.RejectRole)

      hexgeom  = TheSettings.get('ccDlgGeometry')
      tblgeom  = TheSettings.get('ccDlgAddrCols')

      if len(hexgeom) > 0:
         if type(hexgeom) == str:
            geom = QByteArray(bytes.fromhex(hexgeom))
         else:
            geom = hexgeom
         self.restoreGeometry(geom)
      if len(tblgeom) > 0:
         restoreTableView(self.ccView, tblgeom)

      layout = QGridLayout()
      layout.addWidget(lblDescr, 0, 0)
      layout.addWidget(frmCheckAll, 1, 0)
      layout.addWidget(self.ccView, 2, 0)
      layout.addWidget(buttonBox, 5, 0)
      self.setLayout(layout)

      self.setWindowTitle(self.tr('Coin Control (Expert)'))


   #############################################################################
   def getCustomUtxoList(self):
      treeData = self.ccTreeModel.treeStruct.getTreeData()

      utxoList = []
      for section in treeData['UTXO']:
         sectionDict = treeData['UTXO'][section]

         for entry in sectionDict:
            addrList = sectionDict[entry]

            for utxo in addrList:
               if utxo.isChecked():
                  utxoList.append(utxo)

      for addr in treeData['CPFP']:
         for cpfp in treeData['CPFP'][addr]:
            if cpfp.isChecked():
               utxoList.append(cpfp)


      return utxoList

   #############################################################################
   def isUseAllChecked(self):
      return self.useAllCheckBox.isChecked()

   #############################################################################
   def closeEvent(self, event):
      self.saveGeometrySettings()
      super(CoinControlDlg, self).closeEvent(event)

   #############################################################################
   def accept(self, *args):
      self.saveGeometrySettings()
      super(CoinControlDlg, self).accept(*args)

   #############################################################################
   def resetTreeData(self):
      self.ccTreeModel.treeStruct.reset()
      self.ccTreeModel.reset()

   #############################################################################
   def reject(self, *args):
      self.saveGeometrySettings()
      super(CoinControlDlg, self).reject(*args)

   #############################################################################
   def saveGeometrySettings(self):
      if self.isVisible() == True:
         TheSettings.set('ccDlgGeometry', self.saveGeometry().toHex())
         TheSettings.set('ccDlgAddrCols', saveTableView(self.ccView))

   #############################################################################
   def exec_(self):
      return super(CoinControlDlg, self).exec_()

################################################################################
class RBFDlg(ArmoryDialog):

   #############################################################################
   def __init__(self, parent, main, wlt):
      super(RBFDlg, self).__init__(parent, main)

      self.wlt = wlt
      self.rbfTreeModel = RBFTreeModel(self, wlt)
      self.rbfView = QTreeView()
      self.rbfView.setModel(self.rbfTreeModel)

      self.btnAccept = QPushButton(self.tr("Accept"))
      self.btnCancel = QPushButton(self.tr("Cancel"))
      self.btnAccept.clicked.connect(self.accept)
      self.btnCancel.clicked.connect(self.reject)
      buttonBox = QDialogButtonBox()
      buttonBox.addButton(self.btnAccept, QDialogButtonBox.AcceptRole)
      buttonBox.addButton(self.btnCancel, QDialogButtonBox.RejectRole)

      hexgeom  = TheSettings.get('rbfDlgGeometry')
      tblgeom  = TheSettings.get('rbfDlgAddrCols')

      if len(hexgeom) > 0:
         if type(hexgeom) == str:
            geom = QByteArray(bytes.fromhex(hexgeom))
         else:
            geom = hexgeom
         self.restoreGeometry(geom)
      if len(tblgeom) > 0:
         restoreTableView(self.rbfView, tblgeom)

      layout = QGridLayout()
      layout.addWidget(self.rbfView, 1, 0)
      layout.addWidget(buttonBox, 4, 0)
      self.setLayout(layout)

      self.setWindowTitle(self.tr('RBF (Expert)'))

   #############################################################################
   def reject(self, *args):
      self.saveGeometrySettings()
      #self.resetTreeData()
      super(RBFDlg, self).reject(*args)

   #############################################################################
   def saveGeometrySettings(self):
      TheSettings.set('rbfDlgGeometry', self.saveGeometry().toHex())
      TheSettings.set('rbfDlgAddrCols', saveTableView(self.rbfView))

   #############################################################################
   def exec_(self):
      return super(RBFDlg, self).exec_()

   #############################################################################
   def getRBFUtxoList(self):
      treeData = self.rbfTreeModel.treeStruct.getTreeData()

      utxoList = []
      for txHash in treeData:
         entryList = treeData[txHash]
         for entry in entryList:
            if isinstance(entry, list):
               continue

            if entry.isChecked():
               utxoList.append(entry)

      return utxoList

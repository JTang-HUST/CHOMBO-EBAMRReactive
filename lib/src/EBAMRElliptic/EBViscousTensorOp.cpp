#ifdef CH_LANG_CC
/*
 *      _______              __
 *     / ___/ /  ___  __ _  / /  ___
 *    / /__/ _ \/ _ \/  V \/ _ \/ _ \
 *    \___/_//_/\___/_/_/_/_.__/\___/
 *    Please refer to Copyright.txt, in Chombo's root directory.
 */
#endif

#include "EBViscousTensorOp.H"
#include "EBAMRPoissonOpF_F.H"
#include "EBAMRPoissonOp.H"
#include "InterpF_F.H"
#include "EBArith.H"
#include "ViscousTensorOp.H"
#include "ViscousTensorOpF_F.H"
#include "EBViscousTensorOpF_F.H"
#include "EBLevelDataOpsF_F.H"
#include "AMRPoissonOpF_F.H"
#include "EBCoarseAverage.H"
#include "ParmParse.H"
#include "NamespaceHeader.H"
bool EBViscousTensorOp::s_doLazyRelax = false;
bool EBViscousTensorOp::s_turnOffBCs = false; //needs to default to
                                              //false
int EBViscousTensorOp::s_whichLev = -1;
int EBViscousTensorOp::s_step = -1;

bool EBViscousTensorOp::s_forceNoEBCF = true;

IntVect ivVTO(D_DECL(8,12,2));

//-----------------------------------------------------------------------
EBViscousTensorOp::
EBViscousTensorOp(const EBLevelGrid &                                a_eblgFine,
                  const EBLevelGrid &                                a_eblg,
                  const EBLevelGrid &                                a_eblgCoar,
                  const EBLevelGrid &                                a_eblgCoarMG,
                  const Real&                                        a_alpha,
                  const Real&                                        a_beta,
                  const RefCountedPtr<LevelData<EBCellFAB> >&        a_acoef,
                  const RefCountedPtr<LevelData<EBFluxFAB> >&        a_eta,
                  const RefCountedPtr<LevelData<EBFluxFAB> >&        a_lambda,
                  const RefCountedPtr<LevelData<BaseIVFAB<Real> > >& a_etaIrreg,
                  const RefCountedPtr<LevelData<BaseIVFAB<Real> > >& a_lambdaIrreg,
                  const Real&                                        a_dx,
                  const Real&                                        a_dxCoar,
                  const int&                                         a_refToFine,
                  const int&                                         a_refToCoar,
                  const RefCountedPtr<ViscousBaseDomainBC>&          a_domainBC,
                  const RefCountedPtr<ViscousBaseEBBC>&              a_ebBC,
                  const bool&                                        a_hasMGObjects,
                  const IntVect&                                     a_ghostCellsPhi,
                  const IntVect&                                     a_ghostCellsRHS):
  LevelTGAHelmOp<LevelData<EBCellFAB>, EBFluxFAB>(false), // is time-independent
  m_loCFIVS(),
  m_hiCFIVS(),
  m_ghostPhi(a_ghostCellsPhi),
  m_ghostRHS(a_ghostCellsRHS),
  m_eblgFine(a_eblgFine),
  m_eblg(a_eblg),
  m_eblgCoar(a_eblgCoar),
  m_eblgCoarMG(a_eblgCoarMG),
  m_alpha(a_alpha),
  m_beta(a_beta),
  m_alphaDiagWeight(),
  m_betaDiagWeight(),
  m_acoef(a_acoef),
  m_acoef0(),
  m_acoef1(),
  m_eta(a_eta),
  m_lambda(a_lambda),
  m_etaIrreg(a_etaIrreg),
  m_lambdaIrreg(a_lambdaIrreg),
  m_fastFR(),
  m_dx(a_dx),
  m_dxCoar(a_dxCoar),
  m_hasFine(a_eblgFine.isDefined()),
  m_hasCoar(a_eblgCoar.isDefined()),
  m_refToFine(a_refToFine),
  m_refToCoar(a_refToCoar),
  m_hasMGObjects(a_hasMGObjects),
  m_ghostCellsPhi(a_ghostCellsPhi),
  m_ghostCellsRHS(a_ghostCellsRHS),
  m_opEBStencil(),
  m_relCoef(),
  m_grad(),
  m_vofIterIrreg(),
  m_vofIterMulti(),
  m_vofIterDomLo(),
  m_vofIterDomHi(),
  m_interpWithCoarser(),
  m_ebAverage(),
  m_ebAverageMG(),
  m_ebInterp(),
  m_ebInterpMG(),
  m_domainBC(a_domainBC),
  m_ebBC(a_ebBC),
  m_colors()
{
  EBArith::getMultiColors(m_colors);
  if (m_hasCoar)
    {
      ProblemDomain domainCoar = coarsen(m_eblg.getDomain(), m_refToCoar);
      m_interpWithCoarser = RefCountedPtr<EBTensorCFInterp>(new EBTensorCFInterp(m_eblg.getDBL(),  m_eblgCoar.getDBL(),
                                                                                 m_eblg.getEBISL(),m_eblgCoar.getEBISL(),
                                                                                 domainCoar, m_refToCoar, SpaceDim, m_dx,
                                                                                 *m_eblg.getCFIVS()));

      for (int idir = 0; idir < SpaceDim; idir++)
        {
          m_loCFIVS[idir].define(m_eblg.getDBL());
          m_hiCFIVS[idir].define(m_eblg.getDBL());

          for (DataIterator dit = m_eblg.getDBL().dataIterator();  dit.ok(); ++dit)
            {
              m_loCFIVS[idir][dit()].define(m_eblg.getDomain(), m_eblg.getDBL().get(dit()),
                                            m_eblg.getDBL(), idir,Side::Lo);
              m_hiCFIVS[idir][dit()].define(m_eblg.getDomain(), m_eblg.getDBL().get(dit()),
                                            m_eblg.getDBL(), idir,Side::Hi);
            }
        }

      //if this fails, then the AMR grids violate proper nesting.
      ProblemDomain domainCoarsenedFine;
      DisjointBoxLayout dblCoarsenedFine;

      int maxBoxSize = 32;
      bool dumbool;
      bool hasCoarser = EBAMRPoissonOp::getCoarserLayouts(dblCoarsenedFine,
                                                          domainCoarsenedFine,
                                                          m_eblg.getDBL(),
                                                          m_eblg.getEBISL(),
                                                          m_eblg.getDomain(),
                                                          m_refToCoar,
                                                          m_eblg.getEBIS(),
                                                          maxBoxSize, dumbool);

      //should follow from coarsenable
      if (hasCoarser)
        {
          //      EBLevelGrid eblgCoarsenedFine(dblCoarsenedFine, domainCoarsenedFine, 4, Chombo_EBIS::instance());
          EBLevelGrid eblgCoarsenedFine(dblCoarsenedFine, domainCoarsenedFine, 4, m_eblg.getEBIS() );
          m_ebInterp.define( m_eblg.getDBL(),     m_eblgCoar.getDBL(),
                             m_eblg.getEBISL(), m_eblgCoar.getEBISL(),
                             domainCoarsenedFine, m_refToCoar, SpaceDim,
                             m_eblg.getEBIS(),     a_ghostCellsPhi, true, true);
          m_ebAverage.define(m_eblg.getDBL(),   eblgCoarsenedFine.getDBL(),
                             m_eblg.getEBISL(), eblgCoarsenedFine.getEBISL(),
                             domainCoarsenedFine, m_refToCoar, SpaceDim,
                             m_eblg.getEBIS(), a_ghostCellsRHS);

        }
    }
  if (m_hasMGObjects)
    {
      int mgRef = 2;
      m_eblgCoarMG = a_eblgCoarMG;

      m_ebInterpMG.define( m_eblg.getDBL(),   m_eblgCoarMG.getDBL(),
                           m_eblg.getEBISL(), m_eblgCoarMG.getEBISL(),
                           m_eblgCoarMG.getDomain(), mgRef, SpaceDim,
                           m_eblg.getEBIS(),   a_ghostCellsPhi, true, true);
      m_ebAverageMG.define(m_eblg.getDBL(),   m_eblgCoarMG.getDBL(),
                           m_eblg.getEBISL(), m_eblgCoarMG.getEBISL(),
                           m_eblgCoarMG.getDomain() , mgRef, SpaceDim,
                           m_eblg.getEBIS(),   a_ghostCellsRHS);

    }

  defineStencils();
}

/*****/
void
EBViscousTensorOp::
defineStencils()
{
  CH_TIME("ebvto::defineStencils");

  m_divergenceStencil.define(m_eblg.getDBL());
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {

      BaseIVFAB<Real> dumbiv; //no irregular flux for this bit
      Box smallBox = m_eblg.getDBL()[dit()];
      Box grownBox = smallBox;
      grownBox.grow(m_ghostPhi);
      const EBISBox& ebisBox = m_eblg.getEBISL()[dit()];
      EBCellFAB dumEBCF(ebisBox, grownBox,1);
      EBFluxFAB dumEBFF(ebisBox, smallBox,1);
      m_divergenceStencil[dit()] = RefCountedPtr<DivergenceStencil>( new DivergenceStencil(dumEBCF, dumEBFF, dumbiv, smallBox, ebisBox, m_dx*RealVect::Unit, false));
    }

  m_vofIterIrreg.define(m_eblg.getDBL());
  m_vofIterMulti.define(m_eblg.getDBL());
  m_alphaDiagWeight.define(  m_eblg.getDBL());
  m_betaDiagWeight.define(   m_eblg.getDBL());
  LayoutData<BaseIVFAB<VoFStencil> > slowStencil(m_eblg.getDBL());

  EBCellFactory ebcellfact(m_eblg.getEBISL());
  m_relCoef.define(m_eblg.getDBL(), SpaceDim, IntVect::Zero, ebcellfact);
  m_grad.define(m_eblg.getDBL(), SpaceDim*SpaceDim, IntVect::Unit, ebcellfact);
  EBLevelDataOps::setToZero(m_grad);

  Box sideBoxLo[SpaceDim];
  Box sideBoxHi[SpaceDim];
  for (int ivar = 0; ivar < SpaceDim; ivar++)
    {
      int idir = ivar;
      Box domainBox = m_eblg.getDomain().domainBox();
      sideBoxLo[idir] = adjCellLo(domainBox, idir, 1);
      sideBoxLo[idir].shift(idir,  1);
      sideBoxHi[idir] = adjCellHi(domainBox, idir, 1);
      sideBoxHi[idir].shift(idir, -1);
      m_opEBStencil[ivar].define(m_eblg.getDBL());
      m_vofIterDomLo[ivar].define( m_eblg.getDBL()); // vofiterator cache for domain lo
      m_vofIterDomHi[ivar].define( m_eblg.getDBL()); // vofiterator cache for domain hi
    }
  //first define the iterators
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      const EBISBox& ebisBox = m_eblg.getEBISL()[dit()];
      const Box&     grid = m_eblg.getDBL().get(dit());
      //need to grow the irregular set by one near multivalued cells
      IntVectSet ivsIrreg = ebisBox.getIrregIVS(grid);
      IntVectSet ivsMulti = ebisBox.getMultiCells(grid);
      ivsMulti.grow(1);
      ivsMulti &= grid;

      IntVectSet ivsComplement(grid);
      ivsComplement -= ivsMulti;
      ivsComplement -= ivsIrreg;
      //ivscomplement now contains the complement of the cells we need;

      IntVectSet ivsStenc(grid);
      ivsStenc -= ivsComplement;

      m_alphaDiagWeight[dit()].define(ivsStenc, ebisBox.getEBGraph(), SpaceDim);
      m_betaDiagWeight [dit()].define(ivsStenc, ebisBox.getEBGraph(), SpaceDim);
      m_vofIterIrreg   [dit()].define(ivsStenc, ebisBox.getEBGraph()   );
      m_vofIterMulti   [dit()].define(ivsMulti, ebisBox.getEBGraph()   );

      for (int idir = 0; idir < SpaceDim; idir++)
        {
          IntVectSet loIrreg = ivsStenc;
          IntVectSet hiIrreg = ivsStenc;
          loIrreg &= sideBoxLo[idir];
          hiIrreg &= sideBoxHi[idir];
          m_vofIterDomLo[idir][dit()].define(loIrreg,ebisBox.getEBGraph());
          m_vofIterDomHi[idir][dit()].define(hiIrreg,ebisBox.getEBGraph());
        }
      slowStencil[dit()].define(ivsStenc, ebisBox.getEBGraph(), 1);

    }

  Real fakeBeta = 1;
  m_domainBC->setCoef(m_eblg,   fakeBeta,      m_eta,      m_lambda);
  m_ebBC->setCoef(    m_eblg,   fakeBeta,      m_etaIrreg, m_lambdaIrreg);
  m_ebBC->define(    *m_eblg.getCFIVS(), 1.0);

  //now define the stencils for each variable
  for (int ivar = 0; ivar < SpaceDim; ivar++)
    {
      LayoutData<BaseIVFAB<VoFStencil> >* fluxStencil = m_ebBC->getFluxStencil(ivar);
      for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
        {
          const EBISBox& ebisBox = m_eblg.getEBISL()[dit()];
          for (m_vofIterIrreg[dit()].reset(); m_vofIterIrreg[dit()].ok(); ++m_vofIterIrreg[dit()])
            {
              const VolIndex& vof = m_vofIterIrreg[dit()]();
              VoFStencil& vofsten = slowStencil[dit()](vof, 0);
              getVoFStencil(vofsten, vof, dit(), ivar);
              if (fluxStencil != NULL)
                {
                  BaseIVFAB<VoFStencil>& stenFAB = (*fluxStencil)[dit()];
                  //only real irregular cells are going to have EB fluxes
                  //but we are working on a bigger set
                  if (stenFAB.getIVS().contains(vof.gridIndex()))
                    {
                      VoFStencil fluxStencilPt = stenFAB(vof, 0);
                      Real bndryArea = ebisBox.bndryArea(vof);
                      //this might need to get multiplied by the -normal[idir]
                      Real factor = bndryArea/m_dx;
                      fluxStencilPt *= factor;
                      slowStencil[dit()](vof, 0) += fluxStencilPt;
                    }

                }

              Real diagWeight = EBArith::getDiagWeight(slowStencil[dit()](vof, 0), vof, ivar);
              m_betaDiagWeight[dit()](vof, ivar) = diagWeight;
            }

          m_opEBStencil[ivar][dit()] = RefCountedPtr<EBStencil>
            (new EBStencil(m_vofIterIrreg[dit()].getVector(),  slowStencil[dit()],
                           m_eblg.getDBL().get(dit()), m_eblg.getEBISL()[dit()],
                           m_ghostCellsPhi, m_ghostCellsRHS, ivar, true));

        }
    }
  calculateAlphaWeight();
  calculateRelaxationCoefficient();

  m_ivsC2F.resize(SpaceDim);
  const DisjointBoxLayout& grids = m_eblg.getDBL();
  for (int idir = 0; idir < SpaceDim; idir++)m_ivsC2F[idir] = RefCountedPtr< LayoutData<IntVectSet> >(new LayoutData<IntVectSet>(grids));
  const ProblemDomain& domain = m_eblg.getDomain();
  for (DataIterator dit = grids.dataIterator(); dit.ok(); ++dit)
    {
      for (int idir = 0; idir < SpaceDim; idir++)
        {
	  const EBISBox& ebisBox=m_eblg.getEBISL()[dit()];
	  Box ccFluxBox = grids[dit()];
	  ccFluxBox.grow(1);
	  ccFluxBox &= m_eblg.getDomain();
	  IntVectSet ivsIrreg = ebisBox.getIrregIVS(ccFluxBox);
	  Box interiorBox = domain.domainBox();
	  interiorBox.grow(idir, 1);
	  interiorBox &= domain.domainBox();
	  interiorBox.grow(idir, -1);
	  IntVectSet ivsEdge(domain.domainBox());
	  ivsEdge -= interiorBox;
	  ivsEdge &= ccFluxBox;
	  ivsIrreg  |= ivsEdge;
          (*m_ivsC2F[idir])[dit()]=ivsIrreg;
        }
    }

}
//-----------------------------------------------------------------------
EBViscousTensorOp::
EBViscousTensorOp(const EBLevelGrid &                                a_eblgFine,
                  const EBLevelGrid &                                a_eblg,
                  const EBLevelGrid &                                a_eblgCoar,
                  const EBLevelGrid &                                a_eblgCoarMG,
                  const Real&                                        a_alpha,
                  const Real&                                        a_beta,
                  const RefCountedPtr<LevelData<EBCellFAB> >&        a_acoef0,
                  const RefCountedPtr<LevelData<EBCellFAB> >&        a_acoef1,
                  const RefCountedPtr<LevelData<EBCellFAB> >&        a_acoef,
                  const RefCountedPtr<LevelData<EBFluxFAB> >&        a_eta,
                  const RefCountedPtr<LevelData<EBFluxFAB> >&        a_lambda,
                  const RefCountedPtr<LevelData<BaseIVFAB<Real> > >& a_etaIrreg,
                  const RefCountedPtr<LevelData<BaseIVFAB<Real> > >& a_lambdaIrreg,
                  const Real&                                        a_dx,
                  const Real&                                        a_dxCoar,
                  const int&                                         a_refToFine,
                  const int&                                         a_refToCoar,
                  const RefCountedPtr<ViscousBaseDomainBC>&          a_domainBC,
                  const RefCountedPtr<ViscousBaseEBBC>&              a_ebBC,
                  const bool&                                        a_hasMGObjects,
                  const IntVect&                                     a_ghostCellsPhi,
                  const IntVect&                                     a_ghostCellsRHS):
  LevelTGAHelmOp<LevelData<EBCellFAB>, EBFluxFAB>(true), // is time-dependent
  m_loCFIVS(),
  m_hiCFIVS(),
  m_ghostPhi(a_ghostCellsPhi),
  m_ghostRHS(a_ghostCellsRHS),
  m_eblgFine(a_eblgFine),
  m_eblg(a_eblg),
  m_eblgCoar(a_eblgCoar),
  m_eblgCoarMG(a_eblgCoarMG),
  m_alpha(a_alpha),
  m_beta(a_beta),
  m_alphaDiagWeight(),
  m_betaDiagWeight(),
  m_acoef(a_acoef),
  m_acoef0(a_acoef0),
  m_acoef1(a_acoef1),
  m_eta(a_eta),
  m_lambda(a_lambda),
  m_etaIrreg(a_etaIrreg),
  m_lambdaIrreg(a_lambdaIrreg),
  m_fastFR(),
  m_dx(a_dx),
  m_dxCoar(a_dxCoar),
  m_hasFine(a_eblgFine.isDefined()),
  m_hasCoar(a_eblgCoar.isDefined()),
  m_refToFine(a_refToFine),
  m_refToCoar(a_refToCoar),
  m_hasMGObjects(a_hasMGObjects),
  m_ghostCellsPhi(a_ghostCellsPhi),
  m_ghostCellsRHS(a_ghostCellsRHS),
  m_opEBStencil(),
  m_relCoef(),
  m_grad(),
  m_vofIterIrreg(),
  m_vofIterMulti(),
  m_vofIterDomLo(),
  m_vofIterDomHi(),
  m_interpWithCoarser(),
  m_ebAverage(),
  m_ebAverageMG(),
  m_ebInterp(),
  m_ebInterpMG(),
  m_domainBC(a_domainBC),
  m_ebBC(a_ebBC),
  m_colors()
{
  EBArith::getMultiColors(m_colors);

  if (m_hasCoar)
    {
      ProblemDomain domainCoar = coarsen(m_eblg.getDomain(), m_refToCoar);
      m_interpWithCoarser = RefCountedPtr<EBTensorCFInterp>(new EBTensorCFInterp(m_eblg.getDBL(),  m_eblgCoar.getDBL(),
                                                                                 m_eblg.getEBISL(),m_eblgCoar.getEBISL(),
                                                                                 domainCoar, m_refToCoar, SpaceDim, m_dx,
                                                                                 *m_eblg.getCFIVS()));

      for (int idir = 0; idir < SpaceDim; idir++)
        {
          m_loCFIVS[idir].define(m_eblg.getDBL());
          m_hiCFIVS[idir].define(m_eblg.getDBL());

          for (DataIterator dit = m_eblg.getDBL().dataIterator();  dit.ok(); ++dit)
            {
              m_loCFIVS[idir][dit()].define(m_eblg.getDomain(), m_eblg.getDBL().get(dit()),
                                            m_eblg.getDBL(), idir,Side::Lo);
              m_hiCFIVS[idir][dit()].define(m_eblg.getDomain(), m_eblg.getDBL().get(dit()),
                                            m_eblg.getDBL(), idir,Side::Hi);
            }
        }

      //if this fails, then the AMR grids violate proper nesting.
      ProblemDomain domainCoarsenedFine;
      DisjointBoxLayout dblCoarsenedFine;

      int maxBoxSize = 32;
      bool dumbool;
      bool hasCoarser = EBAMRPoissonOp::getCoarserLayouts(dblCoarsenedFine,
                                                          domainCoarsenedFine,
                                                          m_eblg.getDBL(),
                                                          m_eblg.getEBISL(),
                                                          m_eblg.getDomain(),
                                                          m_refToCoar,
                                                          m_eblg.getEBIS(),
                                                          maxBoxSize, dumbool);

      //should follow from coarsenable
      if (hasCoarser)
        {
          //      EBLevelGrid eblgCoarsenedFine(dblCoarsenedFine, domainCoarsenedFine, 4, Chombo_EBIS::instance());
          EBLevelGrid eblgCoarsenedFine(dblCoarsenedFine, domainCoarsenedFine, 4, m_eblg.getEBIS() );
          m_ebInterp.define( m_eblg.getDBL(),     m_eblgCoar.getDBL(),
                             m_eblg.getEBISL(), m_eblgCoar.getEBISL(),
                             domainCoarsenedFine, m_refToCoar, SpaceDim,
                             m_eblg.getEBIS(),     a_ghostCellsPhi, true, true);
          m_ebAverage.define(m_eblg.getDBL(),   eblgCoarsenedFine.getDBL(),
                             m_eblg.getEBISL(), eblgCoarsenedFine.getEBISL(),
                             domainCoarsenedFine, m_refToCoar, SpaceDim,
                             m_eblg.getEBIS(), a_ghostCellsRHS);

        }
    }
  if (m_hasMGObjects)
    {
      int mgRef = 2;
      m_eblgCoarMG = a_eblgCoarMG;

      m_ebInterpMG.define( m_eblg.getDBL(),     m_eblgCoarMG.getDBL(),
                           m_eblg.getEBISL(), m_eblgCoarMG.getEBISL(),
                           m_eblgCoarMG.getDomain(), mgRef, SpaceDim,
                           m_eblg.getEBIS(),   a_ghostCellsPhi, true, true);
      m_ebAverageMG.define(m_eblg.getDBL(),     m_eblgCoarMG.getDBL(),
                           m_eblg.getEBISL(), m_eblgCoarMG.getEBISL(),
                           m_eblgCoarMG.getDomain() , mgRef, SpaceDim,
                           m_eblg.getEBIS(),   a_ghostCellsRHS);

    }

  defineStencils();
}
//-----------------------------------------------------------------------
void
EBViscousTensorOp::
getKappaDivSigmaU(LevelData<EBCellFAB>& a_divSigmaU,
                  const LevelData<EBCellFAB>& a_velocity,
                  const LevelData<EBCellFAB>* a_veloCoar,
                  int a_level)
{
  LevelData<EBCellFAB>& velcast =   (LevelData<EBCellFAB>&) a_velocity;
  if (a_level > 0)
    {
      m_interpWithCoarser->coarseFineInterp(velcast, m_grad, *a_veloCoar);
    }
  this->fillGrad(velcast); //contains an exchange

  EBFluxFactory fact(m_eblg.getEBISL());
  LevelData<EBFluxFAB>     velFlux(m_eblg.getDBL(), SpaceDim, IntVect::Zero, fact);
  LevelData<EBFluxFAB>       sigma(m_eblg.getDBL(), SpaceDim, IntVect::Zero, fact);
  LevelData<EBFluxFAB> velDotSigma(m_eblg.getDBL(),        1,  IntVect::Zero, fact);
  averageCellToFace(velFlux, a_velocity, 0, 0, SpaceDim);

  //so now we have velocity at faces.
  //now we get the viscous flux at faces, dot it with the velocity and take the divergence.
  int ibox=0;
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      EBFluxFAB& thisFlux = sigma[dit()];
      getFlux(thisFlux, a_velocity, m_eblg.getDBL()[dit()], dit(), 1.0); // lucacancel(?) (was a_velocity for velcast)
      ibox++;
    }


  getVelDotSigma(velDotSigma, velFlux, sigma);

  //now we can take the divergence of vel dot sigma and add it to the output
  CH_assert(velDotSigma.ghostVect() == IntVect::Zero);
  CH_assert(a_divSigmaU.ghostVect() == m_ghostPhi);
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      BaseIVFAB<Real> dummy; //no boundary flux here
      m_divergenceStencil[dit()]->divergence(a_divSigmaU[dit()], velDotSigma[dit()], dummy, 0, false);
    }
}
void
EBViscousTensorOp::
pgetKappaDivSigmaU(LevelData<EBCellFAB>& a_divSigmaU,
		   const LevelData<EBCellFAB>& a_velocity,
		   const LevelData<EBCellFAB>* a_veloCoar,
		   const LevelData<EBCellFAB>& pa_velocity,
		   const LevelData<EBCellFAB>* pa_veloCoar,
		   int a_level)
{

  LevelData<EBCellFAB>& velcast =   (LevelData<EBCellFAB>&) a_velocity;
  if (a_level > 0)
    {
      EBLevelDataOps::setToZero(m_grad);
      if (a_veloCoar != NULL)
	m_interpWithCoarser->coarseFineInterp(velcast, m_grad, *a_veloCoar);
    }
  this->fillGrad(velcast); //contains an exchange


  EBFluxFactory fact(m_eblg.getEBISL());
  LevelData<EBFluxFAB>       sigma(m_eblg.getDBL(), SpaceDim, IntVect::Zero, fact);
  LevelData<EBFluxFAB>      sigma1(m_eblg.getDBL(), SpaceDim, IntVect::Zero, fact);
  LevelData<EBFluxFAB>     velFlux(m_eblg.getDBL(), SpaceDim, IntVect::Zero, fact);
  LevelData<EBFluxFAB>    velFlux1(m_eblg.getDBL(), SpaceDim, IntVect::Zero, fact);
  LevelData<EBFluxFAB> velDotSigma(m_eblg.getDBL(),        1,  IntVect::Zero, fact);

  //so now we have velocity at faces.
  //now we get the viscous flux at faces, dot it with the velocity and take the divergence.
  int ibox=0;
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      EBFluxFAB& thisFlux = sigma[dit()];
      getFlux(thisFlux, a_velocity, m_eblg.getDBL()[dit()], dit(), 1.0); // lucacancel(?) (was a_velocity for velcast)
      ibox++;
    }

  LevelData<EBCellFAB>& velcast1 =   (LevelData<EBCellFAB>&) pa_velocity;
  if (a_level > 0)
    {
      EBLevelDataOps::setToZero(m_grad);
      if (pa_veloCoar != NULL)
	m_interpWithCoarser->coarseFineInterp(velcast1, m_grad, *pa_veloCoar);
    }
  this->fillGrad(velcast1); //lucacancel(?) (not sure addition it might be that is evaluated before hand)

  ibox=0;
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      EBFluxFAB& thisFlux = sigma1[dit()];
      getFlux(thisFlux, pa_velocity, m_eblg.getDBL()[dit()], dit(), 1.0); 
      ibox++;
    }

  paverageCellToFace(velFlux, a_velocity, velFlux1, pa_velocity, 0, 0, SpaceDim);


  pgetVelDotSigma(velDotSigma, velFlux, sigma, velFlux1, sigma1);

  //now we can take the divergence of vel dot sigma and add it to the output
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      BaseIVFAB<Real> dummy; //no boundary flux here becasue v=0, v'=0
      m_divergenceStencil[dit()]->divergence(a_divSigmaU[dit()], velDotSigma[dit()], dummy, 0, false);
    }
}
//-----------------------------------------------------------------------
void
EBViscousTensorOp::
getCCSigma(LevelData<EBCellFAB>      & a_sigma,
           const LevelData<EBCellFAB>& a_gradU,
           const LevelData<EBCellFAB>& a_etaCell,
           const LevelData<EBCellFAB>& a_lambdaCell
           )
{
  //Now we loop through all the boxes and do the appropriate multiplication of
  //cofficients with gradients to get sigma
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      const Box& grid = m_eblg.getDBL()[dit()];
      a_sigma[dit()].setVal(0); //needed because this is done through increment
      //first get the eta*(grad u + grad u^T)
      for (int idir = 0; idir < SpaceDim; idir++)
        {
          for (int jdir = 0; jdir < SpaceDim; jdir++)
            {
              int gradcomp = TensorCFInterp::gradIndex(idir, jdir);
              int trancomp = TensorCFInterp::gradIndex(jdir, idir);
              FORT_INCRCCSIGMAETA(CHF_FRA1(        a_sigma[dit()].getSingleValuedFAB(), gradcomp),
                                  CHF_CONST_FRA(   a_gradU[dit()].getSingleValuedFAB()),
                                  CHF_CONST_FRA1(a_etaCell[dit()].getSingleValuedFAB(), 0),
                                  CHF_BOX(grid),
                                  CHF_INT(gradcomp),
                                  CHF_INT(trancomp));
              if (idir == jdir)
                {
                  //now add the lambda*divU bits
                  for (int divDir = 0; divDir < SpaceDim; divDir++)
                    {
                      int diagcomp = TensorCFInterp::gradIndex(divDir, divDir);
                      FORT_INCRCCSIGMALAMBDA(CHF_FRA1(           a_sigma[dit()].getSingleValuedFAB(), gradcomp),
                                             CHF_CONST_FRA(      a_gradU[dit()].getSingleValuedFAB()),
                                             CHF_CONST_FRA1(a_lambdaCell[dit()].getSingleValuedFAB(), 0),
                                             CHF_BOX(grid),
                                             CHF_INT(diagcomp));
                    }
                }
            }

        }
      for (m_vofIterIrreg[dit()].reset(); m_vofIterIrreg[dit()].ok(); ++m_vofIterIrreg[dit()])
        {
          const VolIndex& vof = m_vofIterIrreg[dit()]();
          for (int idir = 0; idir < SpaceDim; idir++)
            {
              for (int jdir = 0; jdir < SpaceDim; jdir++)
                {
                  Real irregSigma = 0;

                  int gradcomp = TensorCFInterp::gradIndex(idir, jdir);
                  int trancomp = TensorCFInterp::gradIndex(jdir, idir);
                  // put in eta(grad u + grad u ^T)
                  irregSigma += a_etaCell[dit()](vof, 0)*(a_gradU[dit()](vof, gradcomp) + a_gradU[dit()](vof, trancomp));

                  if (idir == jdir)
                    {
                      //now add the lambda*divU bits
                      for (int divDir = 0; divDir < SpaceDim; divDir++)
                        {
                          int diagcomp = TensorCFInterp::gradIndex(divDir, divDir);
                          irregSigma += a_lambdaCell[dit()](vof, 0)*a_gradU[dit()](vof, diagcomp);
                        }
                    }
                  a_sigma[dit()](vof, gradcomp) = irregSigma;
                }
            }
        }
    }
}
//----------------------------------------
//simple averaging
void
EBViscousTensorOp::
averageToCells(LevelData<EBCellFAB>&      a_cellCoef,
               const LevelData<EBFluxFAB>& a_faceCoef,
               const LevelData<BaseIVFAB<Real> >& a_irregCoef)
{
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      a_cellCoef[dit()].setVal(0.); //necessary because we are doing increments
      const Box& grid = m_eblg.getDBL()[dit()];
      //number of faces on a regular cell
      Real nfacesper = 2*SpaceDim;
      EBCellFAB&       cellCo = a_cellCoef[dit()];
      for (int idir= 0; idir < SpaceDim; idir++)
        {
          const EBFaceFAB& faceCo = a_faceCoef[dit()][idir];
          FORT_INCRFACETOCELLAVERAGE(CHF_FRA1(      cellCo.getSingleValuedFAB(), 0),
                                     CHF_CONST_FRA1(faceCo.getSingleValuedFAB(), 0),
                                     CHF_INT(idir),
                                     CHF_BOX(grid),
                                     CHF_REAL(nfacesper));
        }
      for (m_vofIterIrreg[dit()].reset(); m_vofIterIrreg[dit()].ok(); ++m_vofIterIrreg[dit()])
        {
          const VolIndex& vof = m_vofIterIrreg[dit()]();
          Real irregCoef = a_irregCoef[dit()](vof, 0);
          int nfaces = 1; //1 is the irregular face
          for (int idir = 0; idir < SpaceDim ; idir++)
            {
              for (SideIterator sit; sit.ok();++sit)
                {
                  Vector<FaceIndex> faces = m_eblg.getEBISL()[dit()].getFaces(vof, idir, sit());
                  nfaces += faces.size();
                  for (int iface = 0; iface < faces.size(); iface++)
                    {
                      irregCoef += a_faceCoef[dit()][idir](faces[iface], 0);
                    }
                }
            }
          irregCoef /= nfaces;   //always >=1 by construction
        }
    }
}
void
EBViscousTensorOp::
getCellCenteredCoefficients(LevelData<EBCellFAB>&    a_etaCell,
                            LevelData<EBCellFAB>& a_lambdaCell)
{
  averageToCells(a_etaCell, *m_eta, *m_etaIrreg);
  averageToCells(a_lambdaCell, *m_lambda, *m_lambdaIrreg);
}
//----------------------------------------
void
EBViscousTensorOp::
getShearStressDotGradU(LevelData<EBCellFAB>      & a_shearStressDotGradUU,
                      const LevelData<EBCellFAB> & a_gradU,
                      int a_level)
{

  EBCellFactory fact(m_eblg.getEBISL());
  LevelData<EBCellFAB>   lambdaCell(m_eblg.getDBL(), SpaceDim         , IntVect::Zero, fact);
  LevelData<EBCellFAB>      etaCell(m_eblg.getDBL(), SpaceDim         , IntVect::Zero, fact);
  LevelData<EBCellFAB>        sigma(m_eblg.getDBL(), SpaceDim*SpaceDim, IntVect::Zero, fact);
  //get coeffcients centered at cells
  getCellCenteredCoefficients(etaCell, lambdaCell);

  //use these to geta  cell-centered sigma
  getCCSigma(sigma, a_gradU, etaCell, lambdaCell);

  //take the dot product of the gradient and the stress
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      const Box& grid = m_eblg.getDBL()[dit()];
      //      const EBISBox& ebisBox = m_eblg.getEBISL()[dit()];
      a_shearStressDotGradUU[dit()].setVal(0.);
      for (int idir = 0; idir < SpaceDim; idir++)
        {
          for (int jdir = 0; jdir < SpaceDim; jdir++)
            {
              int gradComp = TensorCFInterp::gradIndex(idir, jdir);
              FORT_INCRPOINTDOTPROD(CHF_FRA1(a_shearStressDotGradUU[dit()].getSingleValuedFAB(), 0),
                                    CHF_CONST_FRA1(       a_gradU[dit()].getSingleValuedFAB(), gradComp),
                                    CHF_CONST_FRA1(         sigma[dit()].getSingleValuedFAB(), gradComp),
                                    CHF_BOX(grid));
            }
        }
      for (m_vofIterIrreg[dit()].reset(); m_vofIterIrreg[dit()].ok(); ++m_vofIterIrreg[dit()])
        {
          const VolIndex& vof = m_vofIterIrreg[dit()]();

          Real dotprod = 0;
          for (int idir = 0; idir < SpaceDim; idir++)
            {
              for (int jdir = 0; jdir < SpaceDim; jdir++)
                {
                  int gradcomp = TensorCFInterp::gradIndex(idir, jdir);
                  dotprod += sigma[dit()](vof, gradcomp)*a_gradU[dit()](vof, gradcomp);
                }
            }
          a_shearStressDotGradUU[dit()](vof, 0) = dotprod;
        }
    }
}

//--------------------------------------------------------------------
void
EBViscousTensorOp::
getVelDotSigma(LevelData<EBFluxFAB>      & a_velDotSigma,
               const LevelData<EBFluxFAB>& a_vel,
               const LevelData<EBFluxFAB>& a_sigma)
{
  FaceStop::WhichFaces stopCrit = FaceStop::SurroundingWithBoundary;

  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      const Box& grid = m_eblg.getDBL()[dit()];
      const EBISBox& ebisBox = m_eblg.getEBISL()[dit()];
      int idir = 0;
      IntVectSet ivsCell = ebisBox.getIrregIVS(grid);
      for (int faceDir = 0; faceDir < SpaceDim; faceDir++)
        {
          EBFaceFAB&       dotFace = a_velDotSigma[dit()][faceDir];
          const EBFaceFAB& velFace =         a_vel[dit()][faceDir];
          const EBFaceFAB& sigFace =       a_sigma[dit()][faceDir];

          Box faceBox = grid;
          faceBox.surroundingNodes(faceDir);
          FORT_VELDOTSIGMA(CHF_FRA(      dotFace.getSingleValuedFAB()),
                           CHF_CONST_FRA(velFace.getSingleValuedFAB()),
                           CHF_CONST_FRA(sigFace.getSingleValuedFAB()),
                           CHF_BOX(faceBox));

          for (FaceIterator faceit(ivsCell, ebisBox.getEBGraph(), faceDir,stopCrit);
              faceit.ok(); ++faceit)
            {
              const FaceIndex& face = faceit();
              Real dotval = 0;
              for (int velDir = 0; velDir < SpaceDim; velDir++)
                {
                  Real sigmaval = a_sigma[dit()][faceDir](face, velDir);
                  Real   velval =   a_vel[dit()][faceDir](face, velDir);
                  dotval += sigmaval*velval;
                }
              a_velDotSigma[dit()][faceDir](faceit(), 0) = dotval;
            }
          idir++;
        }
    }
}
//--------------------------------------------------------------------
void
EBViscousTensorOp::
pgetVelDotSigma(LevelData<EBFluxFAB>      & a_velDotSigma,
		const LevelData<EBFluxFAB>& a_vel,
		const LevelData<EBFluxFAB>& a_sigma,
		const LevelData<EBFluxFAB>& pa_vel,
		const LevelData<EBFluxFAB>& pa_sigma)
{
  CH_TIME("ebvto::pgetVelDotSigma");
  FaceStop::WhichFaces stopCrit = FaceStop::SurroundingWithBoundary;

  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      const Box& grid = m_eblg.getDBL()[dit()];
      const EBISBox& ebisBox = m_eblg.getEBISL()[dit()];
      int idir = 0;
      IntVectSet ivsCell = ebisBox.getIrregIVS(grid);
      for (int faceDir = 0; faceDir < SpaceDim; faceDir++)
        {
          EBFaceFAB&       dotFace = a_velDotSigma[dit()][faceDir];
          const EBFaceFAB& velFace =         a_vel[dit()][faceDir];
          const EBFaceFAB& sigFace =       a_sigma[dit()][faceDir];
          const EBFaceFAB& velFace1 =       pa_vel[dit()][faceDir];
          const EBFaceFAB& sigFace1 =     pa_sigma[dit()][faceDir];

          Box faceBox = grid;
          faceBox.surroundingNodes(faceDir);
          FORT_PVELDOTSIGMA(CHF_FRA(      dotFace.getSingleValuedFAB()),
			    CHF_CONST_FRA(velFace.getSingleValuedFAB()),
			    CHF_CONST_FRA(sigFace.getSingleValuedFAB()),
			    CHF_CONST_FRA(velFace1.getSingleValuedFAB()),
			    CHF_CONST_FRA(sigFace1.getSingleValuedFAB()),
			    CHF_BOX(faceBox));

          for (FaceIterator faceit(ivsCell, ebisBox.getEBGraph(), faceDir,stopCrit);
              faceit.ok(); ++faceit)
            {
              const FaceIndex& face = faceit();
              Real dotval = 0;
              for (int velDir = 0; velDir < SpaceDim; velDir++)
                {
                  Real sigmaval = a_sigma[dit()][faceDir](face, velDir);
                  Real   velval =   a_vel[dit()][faceDir](face, velDir);
                  Real sigmaval1=pa_sigma[dit()][faceDir](face, velDir);
                  Real   velval1=  pa_vel[dit()][faceDir](face, velDir);
                  dotval += (sigmaval*velval1+sigmaval1*velval);
                }
              a_velDotSigma[dit()][faceDir](faceit(), 0) = dotval;
            }
          idir++;
        }
    }
}

//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
void
EBViscousTensorOp::
finerOperatorChanged(const MGLevelOp<LevelData<EBCellFAB> >& a_operator,
                     int a_coarseningFactor)
{
  const EBViscousTensorOp& op =
    dynamic_cast<const EBViscousTensorOp&>(a_operator);

  // Perform multigrid coarsening on the operator data.
  Interval interv(0, 0); // All data is scalar.
  EBLevelGrid eblgCoar = m_eblg;
  EBLevelGrid eblgFine = op.m_eblg;
  LevelData<EBCellFAB>& acoefCoar = *m_acoef;
  const LevelData<EBCellFAB>& acoefFine = *(op.m_acoef);
  LevelData<EBFluxFAB>& etaCoar = *m_eta;
  const LevelData<EBFluxFAB>& etaFine = *(op.m_eta);
  LevelData<EBFluxFAB>& lambdaCoar = *m_lambda;
  const LevelData<EBFluxFAB>& lambdaFine = *(op.m_lambda);
  LevelData<BaseIVFAB<Real> >& etaCoarIrreg = *m_etaIrreg;
  const LevelData<BaseIVFAB<Real> >& etaFineIrreg = *(op.m_etaIrreg);
  LevelData<BaseIVFAB<Real> >& lambdaCoarIrreg = *m_lambdaIrreg;
  const LevelData<BaseIVFAB<Real> >& lambdaFineIrreg = *(op.m_lambdaIrreg);
  if (a_coarseningFactor != 1)
    {
      EBCoarseAverage averageOp(eblgFine.getDBL(), eblgCoar.getDBL(),
                                eblgFine.getEBISL(), eblgCoar.getEBISL(),
                                eblgCoar.getDomain(), a_coarseningFactor, 1,
                                eblgCoar.getEBIS());

      //MayDay::Warning("might want to figure out what harmonic averaging is in this context");
      averageOp.average(acoefCoar, acoefFine, interv);
      averageOp.average(etaCoar, etaFine, interv);
      averageOp.average(etaCoarIrreg, etaFineIrreg, interv);
      averageOp.average(lambdaCoar, lambdaFine, interv);
      averageOp.average(lambdaCoarIrreg, lambdaFineIrreg, interv);
    }

  // Handle parallel domain ghost elements.
  acoefCoar.exchange(interv);
  etaCoar.exchange(interv);
  lambdaCoar.exchange(interv);
  etaCoarIrreg.exchange(interv);
  lambdaCoarIrreg.exchange(interv);

  // Recompute the relaxation coefficient for the operator.
  calculateAlphaWeight();
  calculateRelaxationCoefficient();

  // Notify any observers of this change.
  notifyObserversOfChange();
}
//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
Real
EBViscousTensorOp::
getSafety()
{
  Real safety = 0.25;
  return safety;
}
//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
void
EBViscousTensorOp::
setTime(Real a_oldTime, Real a_mu, Real a_dt)
{
  // This only affects a coefficients that are time-dependent.
  if (m_acoef0 != NULL)
    {
      CH_assert(m_acoef1 != NULL); // All or nothing!

      // The a coefficient is linearly interpolated as
      // acoef = acoef0 + mu * (acoef1 - acoef0)
      //       = (1 - mu) * acoef0 + mu * acoef1.
      for (DataIterator dit = m_eblg.getDBL().dataIterator();  dit.ok(); ++dit)
        {
          (*m_acoef)[dit()].axby((*m_acoef0)[dit()], (*m_acoef1)[dit()],
                                 1.0 - a_mu, a_mu);
        }

      // Notify any observers that the operator's state has changed.
      notifyObserversOfChange();

      // Redefine the stencils.
      defineStencils();
    }
}
//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
void
EBViscousTensorOp::
setAlphaAndBeta(const Real& a_alpha,
                const Real& a_beta)
{
  CH_TIME("ebvto::setAlphaAndBeta");
  m_alpha = a_alpha;
  m_beta  = a_beta;
  calculateRelaxationCoefficient();
  calculateAlphaWeight(); //have to do this because a coef has probably been changed under us.
}
//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
void
EBViscousTensorOp::
calculateAlphaWeight()
{
  for (DataIterator dit = m_eblg.getDBL().dataIterator();  dit.ok(); ++dit)
    {
      VoFIterator& vofit = m_vofIterIrreg[dit()];
      for (vofit.reset(); vofit.ok(); ++vofit)
        {
          const VolIndex& VoF = vofit();
          Real volFrac = m_eblg.getEBISL()[dit()].volFrac(VoF);
          Real alphaWeight = (*m_acoef)[dit()](VoF, 0);
          alphaWeight *= volFrac;

          for (int ivar = 0; ivar < SpaceDim; ivar++)
            {
              m_alphaDiagWeight[dit()](VoF, ivar) = alphaWeight;
            }
        }
    }
}
//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
void
EBViscousTensorOp::
calculateRelaxationCoefficient()
{
  CH_TIME("ebvto::calcRelCoef");

  //define regular relaxation coefficent
  //define regular relaxation coefficent
  Real safety = getSafety();
  int ncomp = SpaceDim;
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      const Box& grid = m_eblg.getDBL().get(dit());
      const EBCellFAB& acofab = (*m_acoef)[dit()];
      //initialize lambda = alpha*acoef
      m_relCoef[dit()].setVal(0.);
      for (int idir = 0; idir < SpaceDim; idir++)
        {
          m_relCoef[dit()].plus(acofab, 0, idir, 1);
        }
      m_relCoef[dit()]*= m_alpha;

      BaseFab<Real>& regRel =   m_relCoef[dit()].getSingleValuedFAB();

      for (int idir = 0; idir < SpaceDim; idir++)
        {
          FArrayBox& regEta = (FArrayBox &)((*m_eta)   [dit()][idir].getSingleValuedFAB());
          FArrayBox& regLam = (FArrayBox &)((*m_lambda)[dit()][idir].getSingleValuedFAB());
          FORT_DECRINVRELCOEFVTOP(CHF_FRA(regRel),
                                  CHF_FRA(regEta),
                                  CHF_FRA(regLam),
                                  CHF_CONST_REAL(m_beta),
                                  CHF_BOX(grid),
                                  CHF_REAL(m_dx),
                                  CHF_INT(idir),
                                  CHF_INT(ncomp));
        }

      //now invert so lambda = stable lambda for variable coef lapl
      //(according to phil, this is the correct lambda)
      FORT_INVERTLAMBDAVTOP(CHF_FRA(regRel),
                            CHF_REAL(safety),
                            CHF_BOX(grid),
                            CHF_INT(ncomp));

      VoFIterator& vofit = m_vofIterIrreg[dit()];
      for (vofit.reset(); vofit.ok(); ++vofit)
        {
          const VolIndex& VoF = vofit();
          Vector<Real> diagWeight(ncomp, 0);
          Real maxDiag = 0;
          for (int ivar = 0; ivar < ncomp; ivar++)
            {
              Real alphaWeight = m_alphaDiagWeight[dit()](VoF, ivar);
              Real  betaWeight =  m_betaDiagWeight[dit()](VoF, ivar);
              alphaWeight *= m_alpha;
              betaWeight  *= m_beta;

              diagWeight[ivar] = alphaWeight + betaWeight;
              maxDiag =Max(Abs(diagWeight[ivar]), maxDiag);
            }
          for (int ivar = 0; ivar < ncomp; ivar++)
            {
              if (Abs(diagWeight[ivar]) > 1.0e-12)
                {
                  m_relCoef[dit()](VoF, ivar) = safety/diagWeight[ivar];
                }
              else
                {
                  m_relCoef[dit()](VoF, ivar) = 0.0;
                }
            }
        }
    }
}
//-----------------------------------------------------------------------

/*****/
/* generate vof stencil as a divergence of flux stencils */
/***/
void
EBViscousTensorOp::
getVoFStencil(VoFStencil&      a_vofStencil,
              const VolIndex&  a_vof,
              const DataIndex& a_dit,
              int             a_ivar)
{
  const EBISBox& ebisBox = m_eblg.getEBISL()[a_dit];
  a_vofStencil.clear();
  for (int idir = 0; idir < SpaceDim; idir++)
    {
      for (SideIterator sit; sit.ok(); ++sit)
        {
          int isign = sign(sit());
          Vector<FaceIndex> faces = ebisBox.getFaces(a_vof, idir, sit());
          for (int iface = 0; iface < faces.size(); iface++)
            {
              VoFStencil fluxStencil;
              getFluxStencil(fluxStencil, faces[iface], a_dit, a_ivar);
              Real areaFrac = ebisBox.areaFrac(faces[iface]);
              fluxStencil *= Real(isign)*areaFrac/m_dx;
              a_vofStencil += fluxStencil;
            }
        }
    }
}
/***/
/// stencil for flux computation.   the truly ugly part of this computation
/// beta and eta are multiplied in here
/****/
void
EBViscousTensorOp::
getFluxStencil(VoFStencil&      a_fluxStencil,
               const FaceIndex& a_face,
               const DataIndex& a_dit,
               int a_ivar)
{
  //need to do this by interpolating to centroids
  //so get the stencil at each face center and add with
  //interpolation weights
  FaceStencil interpSten = EBArith::getInterpStencil(a_face,
                                                     (*m_eblg.getCFIVS())[a_dit],
                                                     m_eblg.getEBISL()[a_dit],
                                                     m_eblg.getDomain());

  a_fluxStencil.clear();
  for (int isten = 0; isten < interpSten.size(); isten++)
    {
      const FaceIndex& face = interpSten.face(isten);
      const Real&    weight = interpSten.weight(isten);
      VoFStencil faceCentSten;
      getFaceCenteredFluxStencil(faceCentSten, face, a_dit, a_ivar);
      faceCentSten *= weight;
      a_fluxStencil += faceCentSten;
    }
  //debug
  //a_fluxStencil.clear();
  //if (a_face.direction() == 1)  getFaceCenteredFluxStencil(a_fluxStencil, a_face, a_dit, a_ivar);
  //end debug

}
void
EBViscousTensorOp::
getFaceCenteredFluxStencil(VoFStencil&      a_fluxStencil,
                           const FaceIndex& a_face,
                           const DataIndex& a_dit,
                           int a_ivar)
{
  int faceDir= a_face.direction();
  VoFStencil  gBTranSten, gBNormSten;
  //the flux is lambda I diverge(B) + eta(gradB + gradB tran)
  //so the ivar flux for the faceDir direction is
  // lambda*delta(ivar, faceDir)*divergence(B) + eta*(partial(B_ivar, faceDir) + partial(B_faceDir, ivar))

  getGradientStencil(gBNormSten, a_ivar , faceDir, a_face, a_dit);
  getGradientStencil(gBTranSten, faceDir, a_ivar , a_face, a_dit);
  Real etaFace = (*m_eta)[a_dit][faceDir](a_face, 0);
  gBNormSten *=    etaFace;
  gBTranSten *=    etaFace;

  a_fluxStencil += gBNormSten;
  a_fluxStencil += gBTranSten;
  if (a_ivar == faceDir)
    {
      Real lambdaFace = (*m_lambda)[a_dit][faceDir](a_face, 0);
      VoFStencil divergeSten;
      getDivergenceStencil(divergeSten, a_face, a_dit);
      divergeSten   *= lambdaFace;
      a_fluxStencil += divergeSten;
    }
}
/***/
void
EBViscousTensorOp::
getGradientStencil(VoFStencil&  a_gradStencil,
                   int a_ivar,
                   int a_diffDir,
                   const FaceIndex& a_face,
                   const DataIndex& a_dit)
{
  getGradientStencil(a_gradStencil, a_ivar, a_diffDir, a_face, a_dit, m_dx, m_eblg);
}

void
EBViscousTensorOp::
getGradientStencil(VoFStencil&  a_gradStencil,
                   int a_ivar,
                   int a_diffDir,
                   const FaceIndex& a_face,
                   const DataIndex& a_dit,
                   const Real     & a_dx,
                   const EBLevelGrid& a_eblg)
{
  a_gradStencil.clear();
  if ((a_face.direction() == a_diffDir) && (!a_face.isBoundary()))
    {
      a_gradStencil.add(a_face.getVoF(Side::Hi),  1.0/a_dx, a_ivar);
      a_gradStencil.add(a_face.getVoF(Side::Lo), -1.0/a_dx, a_ivar);
    }
  else
    {
      int numGrad = 0;
      for (SideIterator sit; sit.ok(); ++sit)
        {
          const VolIndex& vofSide = a_face.getVoF(sit());
          if (a_eblg.getDomain().contains(vofSide.gridIndex()))
            {
              VoFStencil stenSide;
              IntVectSet& cfivs = (*a_eblg.getCFIVS())[a_dit];
              EBArith::getFirstDerivStencil(stenSide, vofSide,
                                            a_eblg.getEBISL()[a_dit],
                                            a_diffDir, a_dx, &cfivs, a_ivar);
              a_gradStencil += stenSide;
              numGrad++;
            }
        }
      if (numGrad > 1)
        {
          a_gradStencil *= 1.0/Real(numGrad);
        }
    }
}

/***/
void
EBViscousTensorOp::
getDivergenceStencil(VoFStencil&      a_divStencil,
                     const FaceIndex& a_face,
                     const DataIndex& a_dit)
{
  getDivergenceStencil(a_divStencil, a_face, a_dit, m_dx, m_eblg);
}
void
EBViscousTensorOp::
getDivergenceStencil(VoFStencil&      a_divStencil,
                     const FaceIndex& a_face,
                     const DataIndex& a_dit,
                     const Real     & a_dx,
                     const EBLevelGrid& a_eblg)
{
  a_divStencil.clear();
  for (int diffDir = 0; diffDir < SpaceDim; diffDir++)
    {
      VoFStencil diffStencilDir;
      //difference direction and variable are the same
      //because that is the definition of the divergence.
      getGradientStencil(diffStencilDir, diffDir, diffDir, a_face, a_dit, a_dx, a_eblg);
      a_divStencil += diffStencilDir;
    }
}
/***/
EBViscousTensorOp::
~EBViscousTensorOp()
{
}
/***/
void
EBViscousTensorOp::
AMRResidualNC(LevelData<EBCellFAB>&       a_residual,
              const LevelData<EBCellFAB>& a_phiFine,
              const LevelData<EBCellFAB>& a_phi,
              const LevelData<EBCellFAB>& a_rhs,
              bool a_homogeneousBC,
              AMRLevelOp<LevelData<EBCellFAB> >* a_finerOp)
{
  CH_TIME("ebvto::amrresNC");
  //dummy. there is no coarse when this is called
  CH_assert(a_residual.ghostVect() == m_ghostCellsRHS);
  CH_assert(a_rhs.ghostVect() == m_ghostCellsRHS);
  LevelData<EBCellFAB> phiC;
  AMRResidual(a_residual, a_phiFine, a_phi, phiC, a_rhs, a_homogeneousBC, a_finerOp);
}
/***/
void
EBViscousTensorOp::
AMROperatorNC(LevelData<EBCellFAB>&       a_LofPhi,
              const LevelData<EBCellFAB>& a_phiFine,
              const LevelData<EBCellFAB>& a_phi,
              bool a_homogeneousBC,
              AMRLevelOp<LevelData<EBCellFAB> >* a_finerOp)
{
  CH_TIME("ebvto::amropNC");
  //dummy. there is no coarse when this is called
  CH_assert(a_LofPhi.ghostVect() == m_ghostCellsRHS);
  LevelData<EBCellFAB> phiC;
  AMROperator(a_LofPhi, a_phiFine, a_phi, phiC,
              a_homogeneousBC, a_finerOp);
}
/*****/
void
EBViscousTensorOp::
residual(LevelData<EBCellFAB>&       a_residual,
         const LevelData<EBCellFAB>& a_phi,
         const LevelData<EBCellFAB>& a_rhs,
         bool                        a_homogeneousBC)
{
  //this is a multigrid operator so only homogeneous CF BC
  //and null coar level
  CH_TIME("ebvto::residual");
  CH_assert(a_residual.ghostVect() == m_ghostCellsRHS);
  CH_assert(a_phi.ghostVect() == m_ghostCellsPhi);
  applyOp(a_residual,a_phi, a_homogeneousBC);
//  incr(a_residual, a_rhs, -1.0);
//  scale(a_residual, -1.0);
  axby(a_residual,a_residual,a_rhs,-1.0, 1.0);
}

/*****/
void
EBViscousTensorOp::
preCond(LevelData<EBCellFAB>&       a_phi,
        const LevelData<EBCellFAB>& a_rhs)
{
  CH_TIME("ebvto::precond");
  relax(a_phi, a_rhs, 1);
}



/*****/
void
EBViscousTensorOp::
create(LevelData<EBCellFAB>&       a_lhs,
       const LevelData<EBCellFAB>& a_rhs)
{
  CH_TIME("ebvto::create");
  int ncomp = a_rhs.nComp();
  EBCellFactory ebcellfact(m_eblg.getEBISL());
  a_lhs.define(m_eblg.getDBL(), ncomp, a_rhs.ghostVect(), ebcellfact);
  assign(a_lhs, a_rhs);
}

/*****/
void
EBViscousTensorOp::
createCoarsened(LevelData<EBCellFAB>&       a_lhs,
                const LevelData<EBCellFAB>& a_rhs,
                const int&                  a_refRat)
{
  CH_TIME("ebvto::createCoar");
  int ncomp = a_rhs.nComp();
  IntVect ghostVect = a_rhs.ghostVect();

  CH_assert(m_eblg.getDBL().coarsenable(a_refRat));

  //fill ebislayout
  DisjointBoxLayout dblCoarsenedFine;
  coarsen(dblCoarsenedFine, m_eblg.getDBL(), a_refRat);

  EBISLayout ebislCoarsenedFine;
  IntVect ghostVec = a_rhs.ghostVect();
  //const EBIndexSpace* const ebisPtr = Chombo_EBIS::instance();
  ProblemDomain coarDom = coarsen(m_eblg.getDomain(), a_refRat);
  m_eblg.getEBIS()->fillEBISLayout(ebislCoarsenedFine, dblCoarsenedFine, coarDom , ghostVec[0]);
  if (m_refToCoar > 1)
    {
      ebislCoarsenedFine.setMaxRefinementRatio(m_refToCoar, m_eblg.getEBIS());
    }

  //create coarsened data
  EBCellFactory ebcellfactCoarsenedFine(ebislCoarsenedFine);
  a_lhs.define(dblCoarsenedFine, ncomp,ghostVec, ebcellfactCoarsenedFine);
}

/*****/
Real
EBViscousTensorOp::
AMRNorm(const LevelData<EBCellFAB>& a_coarResid,
        const LevelData<EBCellFAB>& a_fineResid,
        const int& a_refRat,
        const int& a_ord)

{
  MayDay::Error("never called");
  return -1.0;
}

/*****/
void
EBViscousTensorOp::
assign(LevelData<EBCellFAB>&       a_lhs,
       const LevelData<EBCellFAB>& a_rhs)
{
  CH_TIME("ebvto::assign");
  EBLevelDataOps::assign(a_lhs,a_rhs);
}

/*****/
Real
EBViscousTensorOp::
dotProduct(const LevelData<EBCellFAB>& a_1,
           const LevelData<EBCellFAB>& a_2)
{
  CH_TIME("ebvto::dotprod");
  ProblemDomain domain;
  Real volume;

  return EBLevelDataOps::kappaDotProduct(volume,a_1,a_2,EBLEVELDATAOPS_ALLVOFS,domain);
}

/*****/
void
EBViscousTensorOp::
incr(LevelData<EBCellFAB>&       a_lhs,
     const LevelData<EBCellFAB>& a_x,
     Real                        a_scale)
{
  CH_TIME("ebvto::incr");
  EBLevelDataOps::incr(a_lhs,a_x,a_scale);
}

/*****/
void
EBViscousTensorOp::
axby(LevelData<EBCellFAB>&       a_lhs,
     const LevelData<EBCellFAB>& a_x,
     const LevelData<EBCellFAB>& a_y,
     Real                        a_a,
     Real                        a_b)
{
  CH_TIME("ebvto::axby");
  EBLevelDataOps::axby(a_lhs,a_x,a_y,a_a,a_b);
}

/*****/
void
EBViscousTensorOp::
scale(LevelData<EBCellFAB>& a_lhs,
      const Real&           a_scale)
{
  CH_TIME("ebvto::scale");
  EBLevelDataOps::scale(a_lhs,a_scale);
}

/*****/
Real
EBViscousTensorOp::
norm(const LevelData<EBCellFAB>& a_rhs,
     int                         a_ord)
{
  CH_TIME("ebvto::norm");

  ProblemDomain domain;
  Real volume, sum;
  //multilevel linearop just wants the vol weighted sum
  IntVectSet ivsExclude;
  RealVect vectDx = m_dx*RealVect::Unit;
  Real max= 0;
  for (int icomp = 0; icomp < SpaceDim; icomp++)
    {
      EBArith::volWeightedSum(sum, volume, a_rhs, m_eblg.getDBL(), m_eblg.getEBISL(), vectDx, ivsExclude, icomp, a_ord);
      if (sum > max)
        max = sum;
    }
  return max;
}

/*****/
void
EBViscousTensorOp::
setToZero(LevelData<EBCellFAB>& a_lhs)
{
  CH_TIME("ebvto::setToZero");
  EBLevelDataOps::setToZero(a_lhs);
}

/*****/
void
EBViscousTensorOp::
setVal(LevelData<EBCellFAB>& a_lhs, const Real& a_value)
{
  CH_TIME("ebvto::setVal");
  EBLevelDataOps::setVal(a_lhs, a_value);
}

void
EBViscousTensorOp::
createCoarser(LevelData<EBCellFAB>&       a_coar,
              const LevelData<EBCellFAB>& a_fine,
              bool                        a_ghosted)
{
  CH_TIME("ebvto::createCoarser");
  const DisjointBoxLayout& dbl = m_eblgCoarMG.getDBL();
  ProblemDomain coarDom = coarsen(m_eblg.getDomain(), 2);

  int nghost = a_fine.ghostVect()[0];
  EBISLayout coarEBISL;

  //  const EBIndexSpace* const ebisPtr = Chombo_EBIS::instance();
  const EBIndexSpace* const ebisPtr = m_eblg.getEBIS();
  ebisPtr->fillEBISLayout(coarEBISL,
                          dbl, coarDom, nghost);

  EBCellFactory ebcellfact(coarEBISL);
  a_coar.define(dbl, SpaceDim,a_fine.ghostVect(),ebcellfact);
}

/*****/
void
EBViscousTensorOp::
relax(LevelData<EBCellFAB>&       a_phi,
      const LevelData<EBCellFAB>& a_rhs,
      int                         a_iterations)
{
  CH_TIME("ebvto::relax");
  CH_assert(a_phi.isDefined());
  CH_assert(a_rhs.isDefined());
  CH_assert(a_phi.ghostVect() >= IntVect::Unit);
  CH_assert(a_phi.nComp() == a_rhs.nComp());
  LevelData<EBCellFAB> lphi;
  create(lphi, a_rhs);
  // do first red, then black passes
  for (int whichIter =0; whichIter < a_iterations; whichIter++)
    {
      for (int icolor = 0; icolor < m_colors.size(); icolor++)
        {
          //after this lphi = L(phi)
          //this call contains bcs and exchange
          if ((icolor == 0) || (!s_doLazyRelax))
            {
              homogeneousCFInterp(a_phi);
              applyOp(  lphi,  a_phi, true);
            }
          gsrbColor(a_phi, lphi, a_rhs, m_colors[icolor]);
        }
    }
}

void
EBViscousTensorOp::
homogeneousCFInterp(LevelData<EBCellFAB>&   a_phi)
{
  CH_TIME("ebvto::homogCFI.1");
  if (m_hasCoar)
    {
      EBCellFactory factCoarse(m_eblgCoar.getEBISL());
      LevelData<EBCellFAB> phiCoarse(m_eblgCoar.getDBL(), a_phi.nComp(), a_phi.ghostVect(), factCoarse);
      EBLevelDataOps::setToZero(phiCoarse);
      cfinterp(a_phi, phiCoarse);
    }
}
/*****/
void
EBViscousTensorOp::
gsrbColor(LevelData<EBCellFAB>&       a_phi,
          const LevelData<EBCellFAB>& a_lph,
          const LevelData<EBCellFAB>& a_rhs,
          const IntVect&              a_color)
{
  CH_TIME("ebvto::gsrbColor");
  const DisjointBoxLayout& dbl = a_phi.disjointBoxLayout();
  for (DataIterator dit = dbl.dataIterator(); dit.ok(); ++dit)
    {
      Box dblBox  = dbl.get(dit());
      BaseFab<Real>&       regPhi =     a_phi[dit()].getSingleValuedFAB();
      const BaseFab<Real>& regLph =     a_lph[dit()].getSingleValuedFAB();
      const BaseFab<Real>& regRhs =     a_rhs[dit()].getSingleValuedFAB();
      const BaseFab<Real>& regRel = m_relCoef[dit()].getSingleValuedFAB();
      IntVect loIV = dblBox.smallEnd();
      IntVect hiIV = dblBox.bigEnd();

      for (int idir = 0; idir < SpaceDim; idir++)
        {
          if (loIV[idir] % 2 != a_color[idir])
            {
              loIV[idir]++;
            }
        }

      if (loIV <= hiIV)
        {
          int ncomp = SpaceDim;
          Box coloredBox(loIV, hiIV);
          FORT_GSRBVTOP(CHF_FRA(regPhi),
                        CHF_CONST_FRA(regLph),
                        CHF_CONST_FRA(regRhs),
                        CHF_CONST_FRA(regRel),
                        CHF_BOX(coloredBox),
                        CHF_CONST_INT(ncomp));
        }

      for (m_vofIterMulti[dit()].reset(); m_vofIterMulti[dit()].ok(); ++m_vofIterMulti[dit()])
        {
          const VolIndex& vof = m_vofIterIrreg[dit()]();
          const IntVect& iv = vof.gridIndex();

          bool doThisVoF = true;
          for (int idir = 0; idir < SpaceDim; idir++)
            {
              if (iv[idir] % 2 != a_color[idir])
                {
                  doThisVoF = false;
                  break;
                }
            }

          if (doThisVoF)
            {
              for (int ivar = 0; ivar < SpaceDim; ivar++)
                {
                  Real resid = a_rhs[dit()](vof, ivar) - a_lph[dit()](vof, ivar);
                  a_phi[dit()](vof, ivar) += m_relCoef[dit()](vof, ivar)*resid;
                }
            }
        }
    }
}
/*****/
void
EBViscousTensorOp::
restrictResidual(LevelData<EBCellFAB>&       a_resCoar,
                 LevelData<EBCellFAB>&       a_phi,
                 const LevelData<EBCellFAB>& a_rhs)
{
  CH_TIME("ebvto::restrictRes");
  LevelData<EBCellFAB> res;
  bool homogeneous = true;
  CH_assert(a_resCoar.ghostVect() == m_ghostRHS);
  CH_assert(a_rhs.ghostVect() == m_ghostRHS);
  create(res, a_rhs);

  // Get the residual on the fine grid
  residual(res,a_phi,a_rhs,homogeneous);

  // now use our nifty averaging operator
  Interval variables(0, SpaceDim-1);
  if (m_hasMGObjects)
    {
      m_ebAverageMG.average(a_resCoar, res, variables);
    }
  else
    {
      m_ebAverage.average(a_resCoar, res, variables);
    }
}

/*****/
void
EBViscousTensorOp::
prolongIncrement(LevelData<EBCellFAB>&       a_phi,
                 const LevelData<EBCellFAB>& a_cor)
{
  CH_TIME("ebvto::prolongInc");
  CH_assert(a_phi.ghostVect() == m_ghostPhi);
  CH_assert(a_cor.ghostVect() == m_ghostPhi);
  Interval vars(0, SpaceDim-1);
  if (m_hasMGObjects)
    {
      m_ebInterpMG.pwlInterp(a_phi, a_cor, vars);
    }
  else
    {
      m_ebInterp.pwlInterp(a_phi, a_cor, vars);
    }
}

/*****/
int
EBViscousTensorOp::
refToCoarser()
{
  return m_refToCoar;
}

/*****/
int
EBViscousTensorOp::
refToFiner()
{
  return m_refToFine;
}

/*****/
void
EBViscousTensorOp::
AMRResidual(LevelData<EBCellFAB>& a_residual,
            const LevelData<EBCellFAB>& a_phiFine,
            const LevelData<EBCellFAB>& a_phi,
            const LevelData<EBCellFAB>& a_phiCoarse,
            const LevelData<EBCellFAB>& a_rhs,
            bool a_homogeneousBC,
            AMRLevelOp<LevelData<EBCellFAB> >* a_finerOp)
{
  CH_TIME("ebvto::amrRes");
  //fillgrad is called in applyop
  this->cfinterp(a_phi, a_phiCoarse);

  applyOp(a_residual, a_phi, a_homogeneousBC);

  if (a_finerOp != NULL)
    {
      reflux(a_phiFine, a_phi,  a_residual, a_finerOp);
    }
  axby(a_residual,a_residual,a_rhs,-1.0, 1.0);
  //  incr(a_residual, a_rhs, -1.0);
  //  scale(a_residual, -1.0);
}

/*****/
void
EBViscousTensorOp::
AMRResidualNF(LevelData<EBCellFAB>& a_residual,
              const LevelData<EBCellFAB>& a_phi,
              const LevelData<EBCellFAB>& a_phiCoarse,
              const LevelData<EBCellFAB>& a_rhs,
              bool a_homogeneousBC)
{
  CH_TIME("ebvto::amrResNF");
  this->cfinterp(a_phi, a_phiCoarse);
  this->residual(a_residual, a_phi, a_rhs, a_homogeneousBC ); //apply boundary conditions
}

/*****/
void
EBViscousTensorOp::
reflux(const LevelData<EBCellFAB>&        a_phiFine,
       const LevelData<EBCellFAB>&        a_phi,
       LevelData<EBCellFAB>&              a_residual,
       AMRLevelOp<LevelData<EBCellFAB> >* a_finerOp)
{
  CH_TIME("ebvto::reflux");
  Interval interv(0,SpaceDim-1);

  int ncomp = SpaceDim;
  if (!m_fastFR.isDefined())
    {
      m_fastFR.define(m_eblgFine, m_eblg, m_refToFine, ncomp, s_forceNoEBCF);
    }

  m_fastFR.setToZero();

  incrementFRCoar(m_fastFR, a_phiFine, a_phi);

  incrementFRFine(m_fastFR, a_phiFine, a_phi, a_finerOp);


  Real scale = 1.0/m_dx;
  m_fastFR.reflux(a_residual, interv, scale);
}

/****/
void
EBViscousTensorOp::
incrementFRCoar(EBFastFR& a_fluxReg,
                const LevelData<EBCellFAB>& a_phiFine,
                const LevelData<EBCellFAB>& a_phi)
{
  CH_TIME("ebvto::incrFRC");
  int ncomp = SpaceDim;
  Interval interv(0,SpaceDim-1);
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      const EBCellFAB& coarfab = a_phi[dit()];
      const EBISBox& ebisBox = m_eblg.getEBISL()[dit()];
      const Box&  box = m_eblg.getDBL().get(dit());
      for (int idir = 0; idir < SpaceDim; idir++)
        {
          //no boundary faces here.

          Box ghostedBox = box;
          ghostedBox.grow(1);
          ghostedBox.grow(idir,-1);
          ghostedBox &= m_eblg.getDomain();

          EBFaceFAB coarflux(ebisBox, ghostedBox, idir, ncomp);

          if (s_forceNoEBCF)
            {
              Box faceBox = surroundingNodes(box, idir);
              FArrayBox& regFlux = (FArrayBox &)     coarflux.getSingleValuedFAB();
              const FArrayBox& regPhi = (const FArrayBox &) coarfab.getSingleValuedFAB();
              const FArrayBox& regGrad = (const FArrayBox &) m_grad[dit()].getSingleValuedFAB();
              getFlux(regFlux, regPhi,  regGrad, faceBox, idir, dit());
            }
          else
           {
              getFlux(coarflux, coarfab, ghostedBox, box, m_eblg.getDomain(), ebisBox, m_dx, dit(), idir);
          }
 
          Real scale = 1.0; //beta and bcoef already in flux
          for (SideIterator sit; sit.ok(); ++sit)
            {
              a_fluxReg.incrementCoarseBoth(coarflux, scale, dit(), interv, idir, sit());
            }
        }
    }
}
/***/
void
EBViscousTensorOp::
incrementFRFine(EBFastFR& a_fluxReg,
                const LevelData<EBCellFAB>& a_phiFine,
                const LevelData<EBCellFAB>& a_phi,
                AMRLevelOp<LevelData<EBCellFAB> >* a_finerOp)
{
  CH_TIME("ebvto::incrFRF");
  int ncomp = SpaceDim;
  Interval interv(0,SpaceDim-1);
  EBViscousTensorOp& finerEBAMROp = (EBViscousTensorOp& )(*a_finerOp);
  //ghost cells of phiFine need to be filled
  LevelData<EBCellFAB>& phiFine = (LevelData<EBCellFAB>&) a_phiFine;


  finerEBAMROp.m_interpWithCoarser->coarseFineInterp(phiFine, finerEBAMROp.m_grad, a_phi);
  phiFine.exchange(interv);
  Real dxFine = m_dx/m_refToFine;
  DataIterator ditf = a_phiFine.dataIterator();
  for (ditf.reset(); ditf.ok(); ++ditf)
    {
      const Box&     boxFine = m_eblgFine.getDBL().get(ditf());
      const EBISBox& ebisBoxFine = m_eblgFine.getEBISL()[ditf()];
      const EBCellFAB& phiFine = a_phiFine[ditf()];

      for (int idir = 0; idir < SpaceDim; idir++)
        {
          for (SideIterator sit; sit.ok(); sit.next())
            {
              //              Box fabBox = adjCellBox(boxFine, idir,        sit(), 1);
              //              fabBox.shift(idir, -sign(sit()));
              Box fabBox = boxFine;

              Box ghostedBox = fabBox;
              ghostedBox.grow(1);
              ghostedBox.grow(idir,-1);
              ghostedBox &= m_eblgFine.getDomain();

              EBFaceFAB fluxFine(ebisBoxFine, ghostedBox, idir, ncomp);

              if (s_forceNoEBCF)
               {
                 Box faceBox = surroundingNodes(fabBox, idir);
                  FArrayBox& regFlux = (FArrayBox &)     fluxFine.getSingleValuedFAB();
                  const FArrayBox& regPhi = (const FArrayBox &) phiFine.getSingleValuedFAB();
                  const FArrayBox& regGrad = (const FArrayBox &) finerEBAMROp.m_grad[ditf()].getSingleValuedFAB();
                  finerEBAMROp.getFlux(regFlux, regPhi,  regGrad, faceBox, idir, ditf());
                }
              else
               {
                 finerEBAMROp.getFlux(fluxFine, phiFine, ghostedBox, fabBox, m_eblgFine.getDomain(),
                                   ebisBoxFine, dxFine, ditf(), idir);
               }

              Real scale = 1.0; //beta and bcoef already in flux

              a_fluxReg.incrementFineBoth(fluxFine, scale, ditf(), interv, idir, sit());
            }
        }
    }
}
void
EBViscousTensorOp::
getFlux(EBFluxFAB&                    a_flux,
        const LevelData<EBCellFAB>&   a_data,
        const Box&                    a_grid,
        const DataIndex&              a_dit,
        Real                          a_scale)
{
  CH_TIME("ebvto::getFlux1");
  a_flux.define(m_eblg.getEBISL()[a_dit], a_grid, SpaceDim);
  a_flux.setVal(0.);
  for (int idir = 0; idir < SpaceDim; idir++)
    {
      Box ghostedBox = a_grid;
      ghostedBox.grow(1);
      ghostedBox.grow(idir,-1);
      ghostedBox &= m_eblg.getDomain();

      getFlux(a_flux[idir],
              a_data[a_dit],
              ghostedBox, a_grid,
              m_eblg.getDomain(),
              m_eblg.getEBISL()[a_dit],
              m_dx, a_dit, idir);
    }
}

void
EBViscousTensorOp::
getFlux(EBFaceFAB&                    a_fluxCentroid,
        const EBCellFAB&              a_phi,
        const Box&                    a_ghostedBox,
        const Box&                    a_fabBox,
        const ProblemDomain&          a_domain,
        const EBISBox&                a_ebisBox,
        const Real&                   a_dx,
        const DataIndex&              a_datInd,
        const int&                    a_idir)
{
  CH_TIME("EBAMRPoissonOp::getFlux2");

  const FArrayBox& regPhi  = (const FArrayBox &)            a_phi.getSingleValuedFAB();
  const FArrayBox& regGrad = (const FArrayBox &) m_grad[a_datInd].getSingleValuedFAB();

  //has some extra cells so...
  a_fluxCentroid.setVal(0.);


  int ncomp = a_phi.nComp();
  CH_assert(ncomp == a_fluxCentroid.nComp());
  Box cellBox = a_ghostedBox;
  //want only interior faces
  cellBox.grow(a_idir, 1);
  cellBox &= a_domain;
  cellBox.grow(a_idir,-1);

  Box faceBox = surroundingNodes(cellBox, a_idir);
  EBFaceFAB fluxCenter(a_ebisBox, a_ghostedBox, a_idir,SpaceDim);
  FArrayBox&       regFlux = (      FArrayBox &)   fluxCenter.getSingleValuedFAB();
  getFlux(regFlux, regPhi,  regGrad, faceBox, a_idir, a_datInd);



  a_fluxCentroid.copy(fluxCenter);

  IntVectSet ivsCell = a_ebisBox.getIrregIVS(cellBox);
  if (!ivsCell.isEmpty())
    {
      FaceStop::WhichFaces stopCrit = FaceStop::SurroundingNoBoundary;

      for (FaceIterator faceit(ivsCell, a_ebisBox.getEBGraph(), a_idir,stopCrit);
          faceit.ok(); ++faceit)
        {
          const FaceIndex& face = faceit();
          for (int ivar = 0; ivar < SpaceDim; ivar++)
            {
              VoFStencil fluxStencil;
              getFluxStencil(fluxStencil, face, a_datInd, ivar);
              //ivar is ignored in this call--vars live in the stencil.
              Real fluxVal =  applyVoFStencil(fluxStencil, a_phi, ivar);
              fluxCenter(faceit(), ivar) = fluxVal;
            }
        }

      //interpolate from face centers to face centroids
      Box cellBox = a_fluxCentroid.getCellRegion();
      EBArith::interpolateFluxToCentroids(a_fluxCentroid,
                                          fluxCenter,
                                          a_fabBox,
                                          a_ebisBox,
                                          a_domain,
                                          a_idir);
    }

  a_fluxCentroid *= m_beta;
}

/*****/
void
EBViscousTensorOp::
AMROperator(LevelData<EBCellFAB>& a_LofPhi,
            const LevelData<EBCellFAB>& a_phiFine,
            const LevelData<EBCellFAB>& a_phi,
            const LevelData<EBCellFAB>& a_phiCoarse,
            bool a_homogeneousBC,
            AMRLevelOp<LevelData<EBCellFAB> >* a_finerOp)
{
  CH_TIME("ebvto::amrOp");
  //fillgrad is called in applyop
  cfinterp(a_phi, a_phiCoarse);

  applyOp(a_LofPhi, a_phi, a_homogeneousBC);
  if (a_finerOp != NULL)
    {
      reflux(a_phiFine, a_phi,  a_LofPhi, a_finerOp);
    }
}

/*****/
void
EBViscousTensorOp::
AMROperatorNF(LevelData<EBCellFAB>& a_LofPhi,
              const LevelData<EBCellFAB>& a_phi,
              const LevelData<EBCellFAB>& a_phiCoarse,
              bool a_homogeneousBC)
{
  CH_TIME("ebvto::amrOpNF");
  //fillgrad is called in applyop
  cfinterp(a_phi, a_phiCoarse);

  //apply boundary conditions in applyOp
  this->applyOp(a_LofPhi, a_phi, a_homogeneousBC );
}
/*****/
void
EBViscousTensorOp::
AMRRestrict(LevelData<EBCellFAB>&       a_resCoar,
            const LevelData<EBCellFAB>& a_residual,
            const LevelData<EBCellFAB>& a_correction,
            const LevelData<EBCellFAB>& a_coarseCorr)
{
  CH_TIME("ebvto::amrRestrict");
  LevelData<EBCellFAB> res;

  EBCellFactory ebcellfactTL(m_eblg.getEBISL());
  IntVect ghostVec = a_residual.ghostVect();

  res.define(m_eblg.getDBL(), SpaceDim, ghostVec, ebcellfactTL);
  EBLevelDataOps::setVal(res, 0.0);

  cfinterp(a_correction, a_coarseCorr);
  //API says that we must average(a_residual - L(correction, coarCorrection))
  applyOp(res, a_correction,  true);
  incr(res, a_residual, -1.0);
  scale(res,-1.0);

  //use our nifty averaging operator
  Interval variables(0, SpaceDim-1);
  m_ebAverage.average(a_resCoar, res, variables);
}
void
EBViscousTensorOp::
AMRProlong(LevelData<EBCellFAB>&       a_correction,
           const LevelData<EBCellFAB>& a_coarseCorr)
{
  CH_TIME("ebvto::amrProlong");
  Interval variables(0, SpaceDim-1);
  m_ebInterp.pwlInterp(a_correction, a_coarseCorr, variables);
}

/*****/
void
EBViscousTensorOp::
AMRUpdateResidual(LevelData<EBCellFAB>&       a_residual,
                  const LevelData<EBCellFAB>& a_correction,
                  const LevelData<EBCellFAB>& a_coarseCorrection)
{
  LevelData<EBCellFAB> r;
  this->create(r, a_residual);
  this->assign(r, a_residual);
  this->AMRResidualNF(r, a_correction, a_coarseCorrection, a_residual, true);
  this->assign(a_residual, r);
}

/*****/
void
EBViscousTensorOp::
applyOp(LevelData<EBCellFAB>&             a_lhs,
        const LevelData<EBCellFAB>&       a_phi,
        bool                              a_homogeneous)
{
  CH_TIME("ebvto::applyOp");
  //contains an exchange
  this->fillGrad(a_phi);

  EBLevelDataOps::setToZero(a_lhs);
  incr( a_lhs, a_phi, m_alpha);
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      for (int idir = 0; idir < SpaceDim; idir++)
        {
          a_lhs[dit()].mult((*m_acoef)[dit()], 0, idir, 1);
        }

      for (int idir = 0; idir < SpaceDim; idir++)
        {
          incrOpRegularDir(a_lhs[dit()], a_phi[dit()], a_homogeneous, idir, dit());
        }
      applyOpIrregular(a_lhs[dit()], a_phi[dit()], a_homogeneous, dit());
    }
}

/*****/
void
EBViscousTensorOp::
getFlux(FArrayBox&                    a_flux,
        const FArrayBox&              a_phi,
        const FArrayBox&              a_gradPhi,
        const Box&                    a_faceBox,
        const int&                    a_idir,
        const DataIndex&              a_datInd)
{
  CH_TIME("ebvto::getFlux3");
  ProblemDomain domain(m_eblg.getDomain());
  Real dx(m_dx);

  CH_assert(a_flux.nComp() == SpaceDim);
  CH_assert( a_phi.nComp() == SpaceDim);

  FArrayBox  faceDiv(a_faceBox, 1);
  FArrayBox  faceGrad(a_faceBox, a_gradPhi.nComp());
  ViscousTensorOp::getFaceDivAndGrad(faceDiv, faceGrad, a_phi, a_gradPhi, domain, a_faceBox, a_idir, dx);
  const FArrayBox& lamFace =(const FArrayBox&)((*m_lambda)[a_datInd][a_idir].getSingleValuedFAB());
  const FArrayBox& etaFace =(const FArrayBox&)((*m_eta   )[a_datInd][a_idir].getSingleValuedFAB());

  ViscousTensorOp::getFluxFromDivAndGrad(a_flux, faceDiv, faceGrad, etaFace, lamFace, a_faceBox, a_idir);
}
/*****/
void
EBViscousTensorOp::
incrOpRegularDir(EBCellFAB&             a_lhs,
                 const EBCellFAB&       a_phi,
                 const bool&            a_homogeneous,
                 const int&             a_dir,
                 const DataIndex&       a_datInd)
{
  CH_TIME("ebvto::incrRegOpDir");
  const Box& grid = m_eblg.getDBL()[a_datInd];
  Box domainFaces = m_eblg.getDomain().domainBox();
  domainFaces.surroundingNodes(a_dir);
  Box interiorFaces = grid;
  interiorFaces.surroundingNodes(a_dir);
  interiorFaces.grow(a_dir, 1);
  interiorFaces &=  domainFaces;
  interiorFaces.grow( a_dir, -1);

  //do flux difference for interior points
  FArrayBox interiorFlux(interiorFaces, SpaceDim);
  const FArrayBox& grad = (FArrayBox&)(m_grad[a_datInd].getSingleValuedFAB());
  const FArrayBox& phi  = (FArrayBox&)(a_phi.getSingleValuedFAB());
  getFlux(interiorFlux, phi,  grad, interiorFaces, a_dir, a_datInd);

  Box loBox, hiBox, centerBox;
  int hasLo, hasHi;
  EBArith::loHiCenter(loBox, hasLo, hiBox, hasHi, centerBox, m_eblg.getDomain(),grid, a_dir);

  //do the low high center thing
  BaseFab<Real>& reglhs        = a_lhs.getSingleValuedFAB();
  Box dummyBox(IntVect::Zero, IntVect::Unit);
  FArrayBox domainFluxLo(dummyBox, SpaceDim);
  FArrayBox domainFluxHi(dummyBox, SpaceDim);

  RealVect origin = RealVect::Zero;
  Real time = 0.0;
  RealVect dxVect = m_dx*RealVect::Unit;
  if (hasLo==1)
    {
      Box loBoxFace = loBox;
      loBoxFace.shiftHalf(a_dir, -1);
      domainFluxLo.resize(loBoxFace, SpaceDim);
      if (!s_turnOffBCs)
        {
	  // LM:: getFaceFluxDivGrad has been implemented only for NeumannViscousDomain..., for the other type this routine defaults to getFaceFlux
	  // where the tangential gradient to the face is set to zero. The irregular cells have not been modifed
	  FArrayBox  faceDiv(loBoxFace, 1);
	  FArrayBox  faceGrad(loBoxFace, grad.nComp());
	  ViscousTensorOp::getFaceDivAndGrad(faceDiv, faceGrad, phi, grad, m_eblg.getDomain(), loBoxFace, a_dir, Real(m_dx));
          m_domainBC->getFaceFluxDivGrad(domainFluxLo,phi, faceDiv, faceGrad, origin,dxVect,a_dir,Side::Lo,a_datInd,time,a_homogeneous);
        }
      else
        {
          //extrapolate to domain flux if there is no bc
          for (BoxIterator boxit(loBoxFace); boxit.ok(); ++boxit)
            {
              const IntVect& iv = boxit();
              IntVect ivn= iv;
              ivn[a_dir]++;
              for (int ivar = 0; ivar < SpaceDim; ivar++)
                {
                  domainFluxLo(iv, ivar) = interiorFlux(ivn, ivar);
                }
            }
        }
    }
  if (hasHi==1)
    {
      Box hiBoxFace = hiBox;
      hiBoxFace.shiftHalf(a_dir, 1);
      domainFluxHi.resize(hiBoxFace, SpaceDim);
      if (!s_turnOffBCs)
        {
	  // LM:: getFaceFluxDivGrad has been implemented only for NeumannViscousDomain..., for the other type this routine defaults to getFaceFlux
	  // where the tangential gradient to the face is set to zero. The irregular cells have not been modifed
	  FArrayBox  faceDiv(hiBoxFace, 1);
	  FArrayBox  faceGrad(hiBoxFace, grad.nComp());
	  ViscousTensorOp::getFaceDivAndGrad(faceDiv, faceGrad, phi, grad, m_eblg.getDomain(), hiBoxFace, a_dir, Real(m_dx));
          m_domainBC->getFaceFluxDivGrad(domainFluxHi,phi, faceDiv, faceGrad, origin,dxVect,a_dir,Side::Hi,a_datInd,time,a_homogeneous);
          //m_domainBC->getFaceFlux(domainFluxHi,phi,origin,dxVect,a_dir,Side::Hi,a_datInd,time,a_homogeneous);
        }
      else
        {
          //extrapolate to domain flux if there is no bc
          //extrapolate to domain flux if there is no bc
          for (BoxIterator boxit(hiBoxFace); boxit.ok(); ++boxit)
            {
              const IntVect& iv = boxit();
              IntVect ivn= iv;
              ivn[a_dir]--;
              for (int ivar = 0; ivar < SpaceDim; ivar++)
                {
                  domainFluxHi(iv, ivar) = interiorFlux(ivn, ivar);
                }
            }
        }
    }
  for (int icomp =0 ; icomp < SpaceDim; icomp++)
    {
      FORT_INCRAPPLYEBVTOP(CHF_FRA1(reglhs,icomp),
                           CHF_CONST_FRA1(interiorFlux, icomp),
                           CHF_CONST_FRA1(domainFluxLo, icomp),
                           CHF_CONST_FRA1(domainFluxHi, icomp),
                           CHF_CONST_REAL(m_beta),
                           CHF_CONST_REAL(m_dx),
                           CHF_BOX(loBox),
                           CHF_BOX(hiBox),
                           CHF_BOX(centerBox),
                           CHF_CONST_INT(hasLo),
                           CHF_CONST_INT(hasHi),
                           CHF_CONST_INT(a_dir));
    }
}
/*****/
void
EBViscousTensorOp::
applyOpIrregular(EBCellFAB&             a_lhs,
                 const EBCellFAB&       a_phi,
                 const bool&            a_homogeneous,
                 const DataIndex&       a_datInd)
{
  CH_TIME("ebvto::incrRegOpIrr");
  RealVect vectDx = m_dx*RealVect::Unit;
  for (int ivar = 0; ivar < SpaceDim; ivar++)
    {
      m_opEBStencil[ivar][a_datInd]->apply(a_lhs, a_phi, m_alphaDiagWeight[a_datInd], m_alpha, m_beta, false);
    }
  if (!a_homogeneous)
    {
      const Real factor = m_beta; ///m_dx;
      m_ebBC->applyEBFlux(a_lhs, a_phi, m_vofIterIrreg[a_datInd], (*m_eblg.getCFIVS()),
                          a_datInd, RealVect::Zero, vectDx, factor,
                          a_homogeneous, 0.0);
    }

  for (int idir = 0; idir < SpaceDim; idir++)
    {
      for (int comp = 0; comp < SpaceDim; comp++)
        {
          for (m_vofIterDomLo[idir][a_datInd].reset(); m_vofIterDomLo[idir][a_datInd].ok();  ++m_vofIterDomLo[idir][a_datInd])
            {
              Real flux;
              const VolIndex& vof = m_vofIterDomLo[idir][a_datInd]();
              m_domainBC->getFaceFlux(flux,vof,comp,a_phi,
                                      RealVect::Zero,vectDx,idir,Side::Lo, a_datInd, 0.0,
                                      a_homogeneous);

              //area gets multiplied in by bc operator
              a_lhs(vof,comp) -= flux*m_beta/m_dx;
            }
          for (m_vofIterDomHi[idir][a_datInd].reset(); m_vofIterDomHi[idir][a_datInd].ok();  ++m_vofIterDomHi[idir][a_datInd])
            {
              Real flux;
              const VolIndex& vof = m_vofIterDomHi[idir][a_datInd]();
              m_domainBC->getFaceFlux(flux,vof,comp,a_phi,
                                      RealVect::Zero,vectDx,idir,Side::Hi,a_datInd,0.0,
                                      a_homogeneous);

              //area gets multiplied in by bc operator
              a_lhs(vof,comp) += flux*m_beta/m_dx;
            }
        }
    }
}
/*****/
void
EBViscousTensorOp::
fillGrad(const LevelData<EBCellFAB>&   a_phi)
{
  CH_TIME("ebvto::fillGrad");
  LevelData<EBCellFAB>& phi = (LevelData<EBCellFAB>&)a_phi;
  phi.exchange(phi.interval());
  const DisjointBoxLayout& grids = a_phi.disjointBoxLayout();
  //compute gradient of phi for parts NOT in ghost cells
  for (DataIterator dit = grids.dataIterator(); dit.ok(); ++dit)
    {
      cellGrad(m_grad[dit()],
               a_phi [dit()],
               grids.get(dit()),
               dit());
    }
  m_grad.exchange();
}
/**/
void
EBViscousTensorOp::
cfinterp(const LevelData<EBCellFAB>&       a_phi,
         const LevelData<EBCellFAB>&       a_phiCoarse)
{
  CH_TIME("ebvto::cfinterp");
  if (m_hasCoar)
    {
      LevelData<EBCellFAB>& phi = (LevelData<EBCellFAB>&)a_phi;
      EBLevelDataOps::setToZero(m_grad);
      if (a_phiCoarse.isDefined())
        {
          m_interpWithCoarser->coarseFineInterp(phi, m_grad, a_phiCoarse);
        }
    }
}
//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
void
EBViscousTensorOp::
cellGrad(EBCellFAB&             a_gradPhi,
         const  EBCellFAB&      a_phi,
         const Box&             a_grid,
         const DataIndex&       a_datInd)
{
  CH_TIME("ebvto::cellgrad");
  CH_assert(a_gradPhi.nComp() == SpaceDim*SpaceDim);
  CH_assert(a_phi.nComp() == SpaceDim);
  const EBISBox& ebisBox = m_eblg.getEBISL()[a_datInd];

  for (int derivDir = 0; derivDir < SpaceDim; derivDir++)
    {
      Box loBox, hiBox, centerBox;
      int hasLo, hasHi;
      EBArith::loHiCenter(loBox, hasLo, hiBox, hasHi, centerBox, m_eblg.getDomain(),a_grid, derivDir);
      for (int phiDir = 0; phiDir < SpaceDim; phiDir++)
        {
          int gradcomp = TensorCFInterp::gradIndex(phiDir,derivDir);
          //
          BaseFab<Real>&       regGrad = a_gradPhi.getSingleValuedFAB();
          const BaseFab<Real>& regPhi  =     a_phi.getSingleValuedFAB();
          FORT_CELLGRADEBVTOP(CHF_FRA1(regGrad, gradcomp),
                              CHF_CONST_FRA1(regPhi, phiDir),
                              CHF_CONST_REAL(m_dx),
                              CHF_BOX(loBox),
                              CHF_BOX(hiBox),
                              CHF_BOX(centerBox),
                              CHF_CONST_INT(hasLo),
                              CHF_CONST_INT(hasHi),
                              CHF_CONST_INT(derivDir));
          {
            CH_TIME("ebvto::fillgrad::irreg");
            for (m_vofIterIrreg[a_datInd].reset(); m_vofIterIrreg[a_datInd].ok(); ++m_vofIterIrreg[a_datInd])
              {
                const VolIndex& vof =  m_vofIterIrreg[a_datInd]();
                Vector<FaceIndex> loFaces = ebisBox.getFaces(vof, derivDir, Side::Lo);
                Vector<FaceIndex> hiFaces = ebisBox.getFaces(vof, derivDir, Side::Hi);
                bool hasLoPt = ((loFaces.size() == 1)  && (!loFaces[0].isBoundary()));
                bool hasHiPt = ((hiFaces.size() == 1)  && (!hiFaces[0].isBoundary()));
                Real valLo=0, valHi=0;
                if (hasLoPt) valLo = a_phi(loFaces[0].getVoF(Side::Lo), phiDir);
                if (hasHiPt) valHi = a_phi(hiFaces[0].getVoF(Side::Hi), phiDir);

                Real valCe = a_phi(vof, phiDir);

                if (hasLoPt && hasHiPt)
                  {
                    a_gradPhi(vof, gradcomp) = (valHi - valLo)/(2.*m_dx);
                  }
                else if (hasHiPt)
                  {
                    a_gradPhi(vof, gradcomp) = (valHi - valCe)/(m_dx);
                  }
                else if (hasLoPt)
                  {
                    a_gradPhi(vof, gradcomp) = (valCe - valLo)/(m_dx);
                  }
                else
                  {
                    a_gradPhi(vof, gradcomp) = 0.0;
                  }
              }
          }
        }
    }
#ifndef NDEBUG
  for (int ivar = 0; ivar < a_gradPhi.nComp(); ivar++)
    {
      a_gradPhi.setCoveredCellVal(ivar, 0.0);
    }
#endif
}

/***/
// luca added routines below

/*****/
void EBViscousTensorOp::averageCellToFace(LevelData<EBFluxFAB>&       a_fluxData,
					  const LevelData<EBCellFAB>& a_cellData,
					  int isrc, int idst, int inco)
{
  const ProblemDomain& domain = m_eblg.getDomain();
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      for (int idir = 0; idir < SpaceDim; idir++)
        {
	  Box grid = m_eblg.getDBL()[dit()];
	  const EBISBox& ebisBox=m_eblg.getEBISL()[dit()];
	  EBFaceFAB&       fluxData = a_fluxData[dit()][idir];
	  const EBCellFAB& cellData = a_cellData[dit()];

	  EBFaceFAB cellCenteredFlux;
	  Box ccFluxBox = grid;
	  ccFluxBox.grow(1);
	  ccFluxBox &= domain;

	  cellCenteredFlux.define(ebisBox, ccFluxBox, idir, fluxData.nComp());
	  cellCenteredFlux.setVal(0.);

	  
	  {//EBLevelDataOps::faceCenteredAverageCellsToFaces 
	    Box faceBox = ccFluxBox;
	    faceBox.surroundingNodes(idir);
	    //do regular cells in fortran for the faces in faceBox
	    for (int icomp = 0; icomp < inco; icomp++)
	      {
		int faceComp= idst + icomp;
		int cellComp= isrc + icomp;
		
		FORT_AVECELLTOFACE(CHF_FRA1(cellCenteredFlux.getSingleValuedFAB(), faceComp),
				   CHF_CONST_FRA1(  cellData.getSingleValuedFAB(), cellComp),
				   CHF_CONST_INT(idir),
				   CHF_BOX(faceBox));
	      }

	    //fix up irregular faces and boundary faces
	    IntVectSet ivsIrreg = (*m_ivsC2F[idir])[dit()];

	    FaceStop::WhichFaces stopCritGrid =  FaceStop::SurroundingWithBoundary;
	    for (FaceIterator faceit(ivsIrreg, ebisBox.getEBGraph(), idir, stopCritGrid); faceit.ok(); ++faceit)
	      {
		const FaceIndex& face = faceit();
		if (!face.isBoundary())
		  {
		    for (int icomp = 0; icomp < inco; icomp++)
		      {
			cellCenteredFlux(face, idst + icomp) = 0.5*(cellData(face.getVoF(Side::Hi), isrc + icomp) +
								    cellData(face.getVoF(Side::Lo), isrc + icomp));
		      }
		  }
		else
		  {
		    for (int icomp = 0; icomp < inco; icomp++)
		      {
			VolIndex whichVoF;
			const Box& domainBox = domain.domainBox();
			if (domainBox.contains(face.gridIndex(Side::Lo)))
			  {
			    whichVoF = face.getVoF(Side::Lo);
			  }
			else if (domainBox.contains(face.gridIndex(Side::Hi)))
			  {
			    whichVoF = face.getVoF(Side::Hi);
			  }
			else
			  {
			    MayDay::Error("face and domain inconsistent:  logic bust in average cells to faces");
			  }
			
			cellCenteredFlux(face, idst + icomp) = cellData(whichVoF, isrc + icomp);
		    }
		  }
	      }
	  } //end of EBLevelDataOps::faceCenteredAverageCellsToFaces 
	   

	  //first copy (this does all the regular cells)
	  Interval srcInt(isrc, isrc+inco-1);
	  Interval dstInt(idst, idst+inco-1);
	  fluxData.copy(grid, dstInt, grid,  cellCenteredFlux, srcInt);

	  //if required, do the fancy interpolation to centroids.
	  IntVectSet ivsIrreg = ebisBox.getIrregIVS(grid);
	  IntVectSet cfivs;
	  FaceStop::WhichFaces stopCritGrid =  FaceStop::SurroundingWithBoundary;
	  for (FaceIterator faceit(ivsIrreg, ebisBox.getEBGraph(), idir, stopCritGrid); faceit.ok(); ++faceit)
	    {
	      FaceStencil sten = EBArith::getInterpStencil(faceit(), cfivs, ebisBox, domain);
	      for (int icomp = 0; icomp < inco; icomp++)
		{
		  sten.setAllVariables(isrc+icomp);
		  fluxData(faceit(), idst+icomp) = applyFaceStencil(sten, cellCenteredFlux, 0);
		}
	    }
	  
        }
    }
}

void EBViscousTensorOp::paverageCellToFace(LevelData<EBFluxFAB>&       a_fluxData,
					   const LevelData<EBCellFAB>& a_cellData,
					   LevelData<EBFluxFAB>&       pa_fluxData,
					   const LevelData<EBCellFAB>& pa_cellData,
					   int isrc, int idst, int inco)
{
  const ProblemDomain& domain = m_eblg.getDomain();
  for (DataIterator dit = m_eblg.getDBL().dataIterator(); dit.ok(); ++dit)
    {
      for (int idir = 0; idir < SpaceDim; idir++)
        {
	  Box grid = m_eblg.getDBL()[dit()];
	  const EBISBox& ebisBox=m_eblg.getEBISL()[dit()];
	  EBFaceFAB&       fluxData = a_fluxData[dit()][idir];
	  const EBCellFAB& cellData = a_cellData[dit()];
	  EBFaceFAB&       fluxData1 = pa_fluxData[dit()][idir];
	  const EBCellFAB& cellData1 = pa_cellData[dit()];

	  EBFaceFAB cellCenteredFlux, cellCenteredFlux1;
	  Box ccFluxBox = grid;
	  ccFluxBox.grow(1);
	  ccFluxBox &= domain;

	  cellCenteredFlux.define(ebisBox, ccFluxBox, idir, fluxData.nComp());
	  cellCenteredFlux.setVal(0.);
	  cellCenteredFlux1.define(ebisBox, ccFluxBox, idir, fluxData.nComp());
	  cellCenteredFlux1.setVal(0.);

	  
	  {//EBLevelDataOps::faceCenteredAverageCellsToFaces 
	    Box faceBox = ccFluxBox;
	    faceBox.surroundingNodes(idir);
	    //do regular cells in fortran for the faces in faceBox
	    for (int icomp = 0; icomp < inco; icomp++)
	      {
		int faceComp= idst + icomp;
		int cellComp= isrc + icomp;
		
		FORT_AVECELLTOFACE(CHF_FRA1(cellCenteredFlux.getSingleValuedFAB(), faceComp),
				   CHF_CONST_FRA1(  cellData.getSingleValuedFAB(), cellComp),
				   CHF_CONST_INT(idir),
				   CHF_BOX(faceBox));
		FORT_AVECELLTOFACE(CHF_FRA1(cellCenteredFlux1.getSingleValuedFAB(), faceComp),
				   CHF_CONST_FRA1(  cellData1.getSingleValuedFAB(), cellComp),
				   CHF_CONST_INT(idir),
				   CHF_BOX(faceBox));
	      }

	    //fix up irregular faces and boundary faces
	    IntVectSet ivsIrreg = (*m_ivsC2F[idir])[dit()];

	    FaceStop::WhichFaces stopCritGrid =  FaceStop::SurroundingWithBoundary;
	    for (FaceIterator faceit(ivsIrreg, ebisBox.getEBGraph(), idir, stopCritGrid); faceit.ok(); ++faceit)
	      {
		const FaceIndex& face = faceit();
		if (!face.isBoundary())
		  {
		    for (int icomp = 0; icomp < inco; icomp++)
		      {
			cellCenteredFlux(face, idst + icomp) = 0.5*(cellData(face.getVoF(Side::Hi), isrc + icomp) +
								    cellData(face.getVoF(Side::Lo), isrc + icomp));
			cellCenteredFlux1(face, idst + icomp) = 0.5*(cellData1(face.getVoF(Side::Hi), isrc + icomp) +
								    cellData1(face.getVoF(Side::Lo), isrc + icomp));
		      }
		  }
		else
		  {
		    for (int icomp = 0; icomp < inco; icomp++)
		      {
			VolIndex whichVoF;
			const Box& domainBox = domain.domainBox();
			if (domainBox.contains(face.gridIndex(Side::Lo)))
			  {
			    whichVoF = face.getVoF(Side::Lo);
			  }
			else if (domainBox.contains(face.gridIndex(Side::Hi)))
			  {
			    whichVoF = face.getVoF(Side::Hi);
			  }
			else
			  {
			    MayDay::Error("face and domain inconsistent:  logic bust in average cells to faces");
			  }
			
			cellCenteredFlux (face, idst + icomp) = cellData (whichVoF, isrc + icomp);
			cellCenteredFlux1(face, idst + icomp) = cellData1(whichVoF, isrc + icomp);
		    }
		  }
	      }
	  } //end of EBLevelDataOps::faceCenteredAverageCellsToFaces 
	   

	  //first copy (this does all the regular cells)
	  Interval srcInt(isrc, isrc+inco-1);
	  Interval dstInt(idst, idst+inco-1);
	  fluxData.copy (grid, dstInt, grid,  cellCenteredFlux,  srcInt);
	  fluxData1.copy(grid, dstInt, grid,  cellCenteredFlux1, srcInt);

	  //if required, do the fancy interpolation to centroids.
	  IntVectSet ivsIrreg = ebisBox.getIrregIVS(grid);
	  IntVectSet cfivs;
	  FaceStop::WhichFaces stopCritGrid =  FaceStop::SurroundingWithBoundary;
	  for (FaceIterator faceit(ivsIrreg, ebisBox.getEBGraph(), idir, stopCritGrid); faceit.ok(); ++faceit)
	    {
	      FaceStencil sten = EBArith::getInterpStencil(faceit(), cfivs, ebisBox, domain);
	      for (int icomp = 0; icomp < inco; icomp++)
		{
		  sten.setAllVariables(isrc+icomp);
		  fluxData (faceit(), idst+icomp) = applyFaceStencil(sten, cellCenteredFlux, 0);
		  fluxData1(faceit(), idst+icomp) = applyFaceStencil(sten, cellCenteredFlux1, 0);
		}
	    }
	  
        }
    }
}
void
EBViscousTensorOp::
printetalambda()
{

  DataIterator ditf = (*m_lambda).dataIterator();
  for (ditf.reset(); ditf.ok(); ++ditf)
    {
      for (int idir = 0; idir < SpaceDim; idir++)
	{
	  const FArrayBox& lamFace =(const FArrayBox&)((*m_lambda)[ditf()][idir].getSingleValuedFAB());
	  const FArrayBox& etaFace =(const FArrayBox&)((*m_eta   )[ditf()][idir].getSingleValuedFAB());
	  pout() << " Max, Min(PrintEtaLambda lamFace) "<< lamFace.max() << lamFace.min() << endl;
	  pout() << " Max, Min(PrintEtaLambda etaFace) "<< etaFace.max() << etaFace.min() << endl;
	}
    }
  
}
//-----------------------------------------------------------------------
//lucaadd
void
EBViscousTensorOp::
dissipation(LevelData<EBCellFAB>&       a_dissipation,
	    const LevelData<EBCellFAB>& a_phi,
	    const LevelData<EBCellFAB>& a_eta,
	    const Real&                 a_lambdafac,
	    LevelData<EBCellFAB>&       TMP)
{
  //contains an exchange
  this->fillGrad(a_phi);

  EBLevelDataOps::setVal(a_dissipation, 0.0);
  for (int derivDir = 0; derivDir < SpaceDim; derivDir++)
    {
      int gradcomp = TensorCFInterp::gradIndex(derivDir,derivDir);
      EBLevelDataOps::product(TMP,m_grad,m_grad,0,gradcomp,gradcomp);
      EBLevelDataOps::incr(a_dissipation,TMP,2.0);
    }
  if ( SpaceDim >= 2 )
    {
      int gradcomp1 = TensorCFInterp::gradIndex(0,1);
      int gradcomp2 = TensorCFInterp::gradIndex(1,0);
      EBLevelDataOps::axby(TMP,m_grad,m_grad,1.0,1.0,0,gradcomp1,gradcomp2);
      EBLevelDataOps::power(TMP,2.0,0);
      EBLevelDataOps::incr(a_dissipation,TMP,1.0);
    }
  if ( SpaceDim >= 3 )
    {
      int gradcomp1 = TensorCFInterp::gradIndex(0,2);
      int gradcomp2 = TensorCFInterp::gradIndex(2,0);
      EBLevelDataOps::axby(TMP,m_grad,m_grad,1.0,1.0,0,gradcomp1,gradcomp2);
      EBLevelDataOps::power(TMP,2.0,0);
      EBLevelDataOps::incr(a_dissipation,TMP,1.0);
      gradcomp1 = TensorCFInterp::gradIndex(1,2);
      gradcomp2 = TensorCFInterp::gradIndex(2,1);
      EBLevelDataOps::axby(TMP,m_grad,m_grad,1.0,1.0,0,gradcomp1,gradcomp2);
      EBLevelDataOps::power(TMP,2.0,0);
      EBLevelDataOps::incr(a_dissipation,TMP,1.0);
    }
  
  //EBLevelDataOps::setVal(TMP, 0.0);
  //averageToCells(TMP, *m_eta, *m_etaIrreg);
  EBLevelDataOps::scale(a_dissipation,a_eta);
  
  EBLevelDataOps::setVal(TMP, 0.0);
  for (int derivDir = 0; derivDir < SpaceDim; derivDir++)
    {
      int gradcomp = TensorCFInterp::gradIndex(derivDir,derivDir);
      EBLevelDataOps::incr(TMP,m_grad,gradcomp,0,1);
    }
  EBLevelDataOps::power(TMP,2.0,0); //div^2
  EBCellFactory fact(m_eblg.getEBISL());
  LevelData<EBCellFAB>   lambdaCell(m_eblg.getDBL(), 1, IntVect::Zero, fact);
  //averageToCells(lambdaCell, *m_lambda, *m_lambdaIrreg);
  EBLevelDataOps::scale(TMP,a_eta); //eta div^2
  EBLevelDataOps::incr(a_dissipation,TMP,a_lambdafac);
  
}
//-----------------------------------------------------------------------
//lucaadd
void
EBViscousTensorOp::
convection(LevelData<EBCellFAB>&       a_convection,
	    const LevelData<EBCellFAB>& a_phi,
	    const LevelData<EBCellFAB>& a_rho,
	    LevelData<EBCellFAB>&       TMP)
{
  //contains an exchange
  this->fillGrad(a_phi);

  EBLevelDataOps::setVal(a_convection, 0.0);
  int phiDir = 0;
  for (int derivDir = 0; derivDir < SpaceDim; derivDir++)
    {
      int gradcomp = TensorCFInterp::gradIndex(phiDir,derivDir);
      EBLevelDataOps::product(TMP,a_phi,m_grad,0,derivDir,gradcomp);
      EBLevelDataOps::incr(a_convection,TMP,1.0);
    }
  EBLevelDataOps::scale(a_convection,a_rho);
}
//lucaadd
void
EBViscousTensorOp::
continuity(LevelData<EBCellFAB>&       a_rhs,
	    const LevelData<EBCellFAB>& a_phi)
{
  //contains an exchange
  this->fillGrad(a_phi);

  EBLevelDataOps::setVal(a_rhs, 0.0);
  for (int derivDir = 0; derivDir < SpaceDim; derivDir++)
    {
      int gradcomp = TensorCFInterp::gradIndex(derivDir,derivDir);
      EBLevelDataOps::assign(a_rhs,m_grad,Interval(derivDir,derivDir),Interval(gradcomp,gradcomp));
      if (derivDir == 1) EBLevelDataOps::scale(a_rhs,-1.0,derivDir);
    }
}
//-----------------------------------------------------------------------
//lucaadd
void
EBViscousTensorOp::
rhoUGradH(LevelData<EBCellFAB>&       a_term,
	  const LevelData<EBCellFAB>& a_phiT,
	  const LevelData<EBCellFAB>& a_phi,
	  const LevelData<EBCellFAB>& a_rho,
	  LevelData<EBCellFAB>&       TMP)
{
  //contains an exchange
  this->fillGrad(a_phiT);

  EBLevelDataOps::setVal(a_term, 0.0);
  int phiDir = 0;
  for (int derivDir = 0; derivDir < SpaceDim; derivDir++)
    {
      int gradcomp = TensorCFInterp::gradIndex(phiDir,derivDir);
      EBLevelDataOps::product(TMP,a_phi,m_grad,0,derivDir,gradcomp);
      EBLevelDataOps::incr(a_term,TMP,0,0,1);
    }
  EBLevelDataOps::scale(a_term,a_rho);
}
bool
EBViscousTensorOp::
checkNAN(const EBFaceFAB& a_data, const Box& a_box, const int a_idir, const string& a_msg)
{
  //set up the data structures
  Real cNAN = 1e40;
  bool dataIsNANINF = false;
  int nCons = a_data.nComp(); 
  FaceStop::WhichFaces stopCritGrid =  FaceStop::SurroundingWithBoundary;
  const EBFaceFAB& dataFaceFAB = a_data;
  const EBISBox& ebisBox = dataFaceFAB.getEBISBox();
  IntVectSet ivs(a_box);
  for (FaceIterator faceit(ivs, ebisBox.getEBGraph(), a_idir, stopCritGrid); faceit.ok(); ++faceit)
    {
      const FaceIndex& face = faceit();
      IntVect gi = face.gridIndex(Side::Lo);
      bool dout = false;
      for (int k=0;k<nCons;k++) dout = dout || ! (abs(dataFaceFAB(face, k))< cNAN);
      if (dout){
	pout() << a_msg << ", " << a_idir << ", " << gi[0] << ", "<< gi[1] << ", "<< gi[2] <<  ", ";		  
	for (int k=0;k<nCons;k++) pout() << dataFaceFAB(face, k) << ", ";
	pout()  << endl;
	dataIsNANINF = true;
      }
    }


  IntVectSet ivsCell = ebisBox.getIrregIVS(a_box);
  if (!ivsCell.isEmpty())
    {

      for (FaceIterator faceit(ivsCell, ebisBox.getEBGraph(), a_idir,stopCritGrid); faceit.ok(); ++faceit)
        {
          const FaceIndex& face = faceit();
	  IntVect gi = face.gridIndex(Side::Lo);
	  bool dout = false;
	  for (int k=0;k<nCons;k++) dout = dout || ! (abs(dataFaceFAB(face, k))< cNAN);
	  if (dout)
	    {
	    pout() << a_msg << "irr, " << a_idir << ", " << gi[0] << ", "<< gi[1] << ", "<< gi[2] <<  ", ";		  
	    for (int k=0;k<nCons;k++) pout() << dataFaceFAB(face, k) << ", ";
	    pout()  << endl;
	    dataIsNANINF = true;
	    }
        }
    }
  return dataIsNANINF;
}

bool
EBViscousTensorOp::
checkNAN(const EBCellFAB&  a_data,  const Box& a_box, const string& a_msg, bool removeIrreg)
{
  bool dataIsNANINF = false;
  Real cNAN = 1e40;
  int nCons = a_data.nComp();  // to avoid checking all the primitive comps
  const EBISBox& ebisBox = a_data.getEBISBox();
  IntVectSet ivs(a_box);
  if(removeIrreg) ivs -= ebisBox.getIrregIVS(a_box);
  for (VoFIterator vofit(ivs, ebisBox.getEBGraph());vofit.ok(); ++vofit)
    {
      const VolIndex& vof = vofit();
      IntVect gi = vof.gridIndex();
      bool dout = false;
      for (int k=0;k<nCons;k++) dout = dout || ! (abs(a_data(vof, k))<cNAN);
      if (dout){
	//if(!dataIsNANINF) m_out << "Box: " << a_box << endl;	
	pout() << a_msg << ", " << gi[0] << ", "<< gi[1] << ", "<< gi[2] <<  ", ";		  
	for (int k=0;k<nCons;k++) pout() << a_data(vof, k) << ", ";
	pout()  << "Covered " << ebisBox.isCovered(gi) << endl;
	dataIsNANINF = true;
      }
    }
  return dataIsNANINF;
}

Vector<Real>
EBViscousTensorOp::
pickNAN(const EBFaceFAB& a_data, const IntVect& a_iv, int& a_dir)
{  
  Vector<Real> retval;
  if(!a_data.getRegion().contains(a_iv))return(retval);
  VolIndex vof = VolIndex(a_iv,0);
  const EBISBox& ebisBox = a_data.getEBISBox();
  int nF = (ebisBox.numFaces(vof,a_dir,Side::Lo) + ebisBox.numFaces(vof,a_dir,Side::Hi));
  retval.resize(nF,0);int ifn=0;
  for (SideIterator sit; sit.ok(); ++sit)
    {
      Vector<FaceIndex> faces = ebisBox.getFaces(vof,a_dir,sit());
      for (int k = 0; k < faces.size(); k++)
	{retval[ifn] = a_data(faces[k], 0);ifn++;}
    }
  
  return retval;

}
void EBViscousTensorOp:: printNAN(const FArrayBox& a_var, const string& a_msg)
{
  if(a_var.box().contains(ivVTO))
    {
      pout() << ivVTO << " NAN " << a_msg  << " " << a_var.get(ivVTO,0) << endl;
    }
}
#include "NamespaceFooter.H"

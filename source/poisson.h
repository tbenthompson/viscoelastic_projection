#ifndef __viscosaur_poisson_h
#define __viscosaur_poisson_h
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/timer.h>

#include <deal.II/lac/generic_linear_algebra.h>

// On my current machine, Trilinos linear algebra seems to be
// about twice as fast as PETSc. This is probably an artifact of some 
// configurations, so flip this flag to try out PETSc (assuming it's
// installed and deal.II is configured to use it). 
// #define USE_PETSC_LA

namespace LA
{
#ifdef USE_PETSC_LA
    using namespace dealii::LinearAlgebraPETSc;
#else
    using namespace dealii::LinearAlgebraTrilinos;
#endif
}

#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/lac/compressed_simple_sparsity_pattern.h>

#include <deal.II/lac/petsc_parallel_sparse_matrix.h>
#include <deal.II/lac/petsc_parallel_vector.h>
#include <deal.II/lac/petsc_solver.h>
#include <deal.II/lac/petsc_precondition.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>

#include <deal.II/base/utilities.h>
#include <deal.II/base/conditional_ostream.h>
#include <deal.II/base/index_set.h>
#include <deal.II/lac/sparsity_tools.h>
#include <deal.II/distributed/tria.h>
#include <deal.II/distributed/grid_refinement.h>

#include <fstream>
#include <iostream>

#include "analytic.h"

// TODO: Clean up this header file. Pimpl it.
namespace viscosaur
{
    using namespace dealii;

    class OutputHandler
    {

    }

    /*
     * The Poisson Solver. Most of this code is extracted from tutorial 40
     * on the deal.ii website. Currently located at
     * http://www.dealii.org/8.1.0/doxygen/deal.II/step_40.html 
     *
     * Add some documentation...
     *
     * Note that this entire class is defined in the header. This is required
     * for a templated class. C++11 may have fixed this. Check?
     */
    template <int dim>
    class Poisson
    {
        public:
            Poisson ();
            ~Poisson ();

            void run ();

        private:
            void setup_system ();
            void assemble_system ();
            void solve ();
            void refine_grid ();
            void output_results (const unsigned int cycle) const;
            void init_mesh ();

            MPI_Comm              mpi_communicator;
            parallel::distributed::Triangulation<dim> triangulation;
            DoFHandler<dim>       dof_handler;
            FE_Q<dim>             fe;
            IndexSet              locally_owned_dofs;
            IndexSet              locally_relevant_dofs;
            ConstraintMatrix      constraints;
            LA::MPI::SparseMatrix system_matrix;
            LA::MPI::Vector       locally_relevant_solution;
            LA::MPI::Vector       system_rhs;
            ConditionalOStream    pcout;
            TimerOutput           computing_timer;
    };


    template <int dim>
    Poisson<dim>::Poisson ()
    :
        mpi_communicator (MPI_COMM_WORLD),
        triangulation (mpi_communicator,
                typename Triangulation<dim>::MeshSmoothing
                (Triangulation<dim>::smoothing_on_refinement |
                 Triangulation<dim>::smoothing_on_coarsening)),
        dof_handler (triangulation),
        fe (2),
        pcout (std::cout,
                (Utilities::MPI::this_mpi_process(mpi_communicator)
                 == 0)),
        computing_timer (pcout,
                TimerOutput::summary,
                TimerOutput::wall_times)
    {
        pcout << "Setting up the Poisson solver." << std::endl;
    }



    template <int dim>
    Poisson<dim>::~Poisson ()
    {
        dof_handler.clear ();
    }



    template <int dim>
    void Poisson<dim>::setup_system ()
    {
        TimerOutput::Scope t(computing_timer, "setup");

        dof_handler.distribute_dofs (fe);

        locally_owned_dofs = dof_handler.locally_owned_dofs ();
        DoFTools::extract_locally_relevant_dofs (dof_handler,
                locally_relevant_dofs);

        locally_relevant_solution.reinit (locally_owned_dofs,
                locally_relevant_dofs, mpi_communicator);
        system_rhs.reinit (locally_owned_dofs, mpi_communicator);

        system_rhs = 0;

        constraints.clear ();
        constraints.reinit (locally_relevant_dofs);
        DoFTools::make_hanging_node_constraints (dof_handler, constraints);
        VectorTools::interpolate_boundary_values (dof_handler,
                0,
                ZeroFunction<dim>(),
                constraints);
        constraints.close ();

        CompressedSimpleSparsityPattern csp (locally_relevant_dofs);

        DoFTools::make_sparsity_pattern (dof_handler, csp,
                constraints, false);
        SparsityTools::distribute_sparsity_pattern (csp,
                dof_handler.n_locally_owned_dofs_per_processor(),
                mpi_communicator,
                locally_relevant_dofs);

        system_matrix.reinit (locally_owned_dofs,
                locally_owned_dofs,
                csp,
                mpi_communicator);
    }




    template <int dim>
    void Poisson<dim>::assemble_system ()
    {
        TimerOutput::Scope t(computing_timer, "assembly");

        const QGauss<dim>  quadrature_formula(3);

        FEValues<dim> fe_values (fe, quadrature_formula,
                update_values    |  update_gradients |
                update_quadrature_points |
                update_JxW_values);

        const unsigned int   dofs_per_cell = fe.dofs_per_cell;
        const unsigned int   n_q_points    = quadrature_formula.size();

        FullMatrix<double>   cell_matrix (dofs_per_cell, dofs_per_cell);
        Vector<double>       cell_rhs (dofs_per_cell);

        std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell);

        typename DoFHandler<dim>::active_cell_iterator
            cell = dof_handler.begin_active(),
                 endc = dof_handler.end();
        for (; cell!=endc; ++cell)
        {
            if (cell->is_locally_owned())
            {
                cell_matrix = 0;
                cell_rhs = 0;

                fe_values.reinit (cell);

                for (unsigned int q_point=0; q_point<n_q_points; ++q_point)
                {
                    const double
                        rhs_value
                        = (fe_values.quadrature_point(q_point)[1]
                                >
                                0.5+0.25*std::sin(4.0 * numbers::PI *
                                    fe_values.quadrature_point(q_point)[0])
                                ? 1 : -1);

                    for (unsigned int i=0; i<dofs_per_cell; ++i)
                    {
                        for (unsigned int j=0; j<dofs_per_cell; ++j)
                            cell_matrix(i,j) += (fe_values.shape_grad(i,q_point) *
                                    fe_values.shape_grad(j,q_point) *
                                    fe_values.JxW(q_point));

                        cell_rhs(i) += (rhs_value *
                                fe_values.shape_value(i,q_point) *
                                fe_values.JxW(q_point));
                    }
                }

                cell->get_dof_indices (local_dof_indices);
                constraints.distribute_local_to_global (cell_matrix,
                        cell_rhs,
                        local_dof_indices,
                        system_matrix,
                        system_rhs);
            }
        }

        system_matrix.compress (VectorOperation::add);
        system_rhs.compress (VectorOperation::add);
    }




    template <int dim>
    void Poisson<dim>::solve ()
    {
        TimerOutput::Scope t(computing_timer, "solve");
        LA::MPI::Vector
            completely_distributed_solution (locally_owned_dofs, mpi_communicator);

        SolverControl solver_control (dof_handler.n_dofs(), 1e-12);

        LA::SolverCG solver(solver_control, mpi_communicator);
        LA::MPI::PreconditionAMG preconditioner;

        LA::MPI::PreconditionAMG::AdditionalData data;

#ifdef USE_PETSC_LA
        data.symmetric_operator = true;
#else
#endif
        preconditioner.initialize(system_matrix, data);

        solver.solve (system_matrix, completely_distributed_solution, system_rhs,
                preconditioner);

        pcout << "   Solved in " << solver_control.last_step()
              << " iterations." << std::endl;

        constraints.distribute (completely_distributed_solution);

        locally_relevant_solution = completely_distributed_solution;
    }




    template <int dim>
    void Poisson<dim>::refine_grid ()
    {
        TimerOutput::Scope t(computing_timer, "refine");

        Vector<float> estimated_error_per_cell (triangulation.n_active_cells());
        KellyErrorEstimator<dim>::estimate (dof_handler,
                QGauss<dim-1>(3),
                typename FunctionMap<dim>::type(),
                locally_relevant_solution,
                estimated_error_per_cell);
        parallel::distributed::GridRefinement::
            refine_and_coarsen_fixed_number (triangulation,
                    estimated_error_per_cell,
                    0.3, 0.03);
        triangulation.execute_coarsening_and_refinement ();
    }




    template <int dim>
    void Poisson<dim>::output_results (const unsigned int cycle) const
    {
        DataOut<dim> data_out;
        data_out.attach_dof_handler (dof_handler);
        data_out.add_data_vector (locally_relevant_solution, "u");

        Vector<float> subdomain (triangulation.n_active_cells());
        for (unsigned int i=0; i<subdomain.size(); ++i)
            subdomain(i) = triangulation.locally_owned_subdomain();
        data_out.add_data_vector (subdomain, "subdomain");

        data_out.build_patches ();

        const std::string filename = ("solution-" +
                Utilities::int_to_string (cycle, 2) +
                "." +
                Utilities::int_to_string
                (triangulation.locally_owned_subdomain(), 4));
        std::ofstream output ((filename + ".vtu").c_str());
        data_out.write_vtu (output);

        if (Utilities::MPI::this_mpi_process(mpi_communicator) == 0)
        {
            std::vector<std::string> filenames;
            for (unsigned int i=0;
                    i<Utilities::MPI::n_mpi_processes(mpi_communicator);
                    ++i)
                filenames.push_back ("solution-" +
                        Utilities::int_to_string (cycle, 2) +
                        "." +
                        Utilities::int_to_string (i, 4) +
                        ".vtu");

            std::ofstream master_output ((filename + ".pvtu").c_str());
            data_out.write_pvtu_record (master_output, filenames);
        }
    }



    template <int dim>
    void Poisson<dim>::init_mesh ()
    {
        GridGenerator::hyper_cube (triangulation);
        triangulation.refine_global (5);
    }

    template <int dim>
    void Poisson<dim>::run ()
    {
        const unsigned int n_cycles = 10;
        for (unsigned int cycle=0; cycle<n_cycles; ++cycle)
        {
            pcout << "Cycle " << cycle << ':' << std::endl;

            if (cycle == 0)
            {
                init_mesh ();
            }
            else
                refine_grid ();

            setup_system ();

            pcout << "   Number of active cells:       "
                << triangulation.n_global_active_cells()
                << std::endl
                << "   Number of degrees of freedom: "
                << dof_handler.n_dofs()
                << std::endl;

            assemble_system ();
            solve ();

            if (Utilities::MPI::n_mpi_processes(mpi_communicator) <= 32)
            {
                TimerOutput::Scope t(computing_timer, "output");
                // output_results (cycle);
            }

            pcout << std::endl;
            computing_timer.print_summary ();
            computing_timer.reset ();
        }

    }
}
#endif
#ifndef SPIRIT_USE_CUDA

#include <engine/Hamiltonian_Heisenberg.hpp>
#include <engine/Vectormath.hpp>
#include <engine/Neighbours.hpp>
#include <data/Spin_System.hpp>
#include <utility/Constants.hpp>
#include <algorithm>

#include <Eigen/Dense>
#include <Eigen/Core>


using namespace Data;
using namespace Utility;
namespace C = Utility::Constants;
using Engine::Vectormath::check_atom_type;
using Engine::Vectormath::idx_from_pair;
using Engine::Vectormath::idx_from_tupel;


namespace Engine
{
    // Construct a Heisenberg Hamiltonian with pairs
    Hamiltonian_Heisenberg::Hamiltonian_Heisenberg(
        scalar external_field_magnitude, Vector3 external_field_normal,
        intfield anisotropy_indices, scalarfield anisotropy_magnitudes, vectorfield anisotropy_normals,
        pairfield exchange_pairs, scalarfield exchange_magnitudes,
        pairfield dmi_pairs, scalarfield dmi_magnitudes, vectorfield dmi_normals,
        DDI_Method ddi_method, intfield ddi_n_periodic_images, scalar ddi_radius,
        tripletfield triplets, scalarfield triplet_magnitudes1, scalarfield triplet_magnitudes2, 
        quadrupletfield quadruplets, scalarfield quadruplet_magnitudes,
        std::shared_ptr<Data::Geometry> geometry,
        intfield boundary_conditions
    ) :
        Hamiltonian(boundary_conditions),
        geometry(geometry),
        external_field_magnitude(external_field_magnitude * C::mu_B), external_field_normal(external_field_normal),
        anisotropy_indices(anisotropy_indices), anisotropy_magnitudes(anisotropy_magnitudes), anisotropy_normals(anisotropy_normals),
        exchange_pairs_in(exchange_pairs), exchange_magnitudes_in(exchange_magnitudes), exchange_shell_magnitudes(0),
        dmi_pairs_in(dmi_pairs), dmi_magnitudes_in(dmi_magnitudes), dmi_normals_in(dmi_normals), dmi_shell_magnitudes(0), dmi_shell_chirality(0),
        triplets(triplets), triplet_magnitudes1(triplet_magnitudes1), triplet_magnitudes2(triplet_magnitudes2), 
        quadruplets(quadruplets), quadruplet_magnitudes(quadruplet_magnitudes),
        ddi_method(ddi_method), ddi_n_periodic_images(ddi_n_periodic_images), ddi_cutoff_radius(ddi_radius)
    {
        // Generate interaction pairs, constants etc.
        this->Update_Interactions();
    }

    // Construct a Heisenberg Hamiltonian from shells
    Hamiltonian_Heisenberg::Hamiltonian_Heisenberg(
        scalar external_field_magnitude, Vector3 external_field_normal,
        intfield anisotropy_indices, scalarfield anisotropy_magnitudes, vectorfield anisotropy_normals,
        scalarfield exchange_shell_magnitudes,
        scalarfield dmi_shell_magnitudes, int dm_chirality,
        DDI_Method ddi_method, intfield ddi_n_periodic_images, scalar ddi_radius,
        tripletfield triplets, scalarfield triplet_magnitudes1, scalarfield triplet_magnitudes2, 
        quadrupletfield quadruplets, scalarfield quadruplet_magnitudes,
        std::shared_ptr<Data::Geometry> geometry,
        intfield boundary_conditions
    ) :
        Hamiltonian(boundary_conditions),
        geometry(geometry),
        external_field_magnitude(external_field_magnitude * C::mu_B), external_field_normal(external_field_normal),
        anisotropy_indices(anisotropy_indices), anisotropy_magnitudes(anisotropy_magnitudes), anisotropy_normals(anisotropy_normals),
        exchange_pairs_in(0), exchange_magnitudes_in(0), exchange_shell_magnitudes(exchange_shell_magnitudes),
        dmi_pairs_in(0), dmi_magnitudes_in(0), dmi_normals_in(0), dmi_shell_magnitudes(dmi_shell_magnitudes), dmi_shell_chirality(dm_chirality),
        triplets(triplets), triplet_magnitudes1(triplet_magnitudes1), triplet_magnitudes2(triplet_magnitudes2), 
        quadruplets(quadruplets), quadruplet_magnitudes(quadruplet_magnitudes),
        ddi_method(ddi_method), ddi_n_periodic_images(ddi_n_periodic_images), ddi_cutoff_radius(ddi_radius)
    {
        // Generate interaction pairs, constants etc.
        this->Update_Interactions();
    }

    void Hamiltonian_Heisenberg::Update_Interactions()
    {
        Clean_DDI();
        #if defined(SPIRIT_USE_OPENMP)
        // When parallelising (cuda or openmp), we need all neighbours per spin
        const bool use_redundant_neighbours = true;
        #else
        // When running on a single thread, we can ignore redundant neighbours
        const bool use_redundant_neighbours = false;
        #endif

        // Exchange
        this->exchange_pairs      = pairfield(0);
        this->exchange_magnitudes = scalarfield(0);
        if( exchange_shell_magnitudes.size() > 0 )
        {
            // Generate Exchange neighbours
            intfield exchange_shells(0);
            Neighbours::Get_Neighbours_in_Shells(*geometry, exchange_shell_magnitudes.size(), exchange_pairs, exchange_shells, use_redundant_neighbours);
            for (unsigned int ipair = 0; ipair < exchange_pairs.size(); ++ipair)
            {
                this->exchange_magnitudes.push_back(exchange_shell_magnitudes[exchange_shells[ipair]]);
            }
        }
        else
        {
            // Use direct list of pairs
            this->exchange_pairs      = this->exchange_pairs_in;
            this->exchange_magnitudes = this->exchange_magnitudes_in;
            if( use_redundant_neighbours )
            {
                for (int i = 0; i < exchange_pairs_in.size(); ++i)
                {
                    auto& p = exchange_pairs_in[i];
                    auto& t = p.translations;
                    this->exchange_pairs.push_back(Pair{p.j, p.i, {-t[0], -t[1], -t[2]}});
                    this->exchange_magnitudes.push_back(exchange_magnitudes_in[i]);
                }
            }
        }

        // DMI
        this->dmi_pairs      = pairfield(0);
        this->dmi_magnitudes = scalarfield(0);
        this->dmi_normals    = vectorfield(0);
        if( dmi_shell_magnitudes.size() > 0 )
        {
            // Generate DMI neighbours and normals
            intfield dmi_shells(0);
            Neighbours::Get_Neighbours_in_Shells(*geometry, dmi_shell_magnitudes.size(), dmi_pairs, dmi_shells, use_redundant_neighbours);
            for (unsigned int ineigh = 0; ineigh < dmi_pairs.size(); ++ineigh)
            {
                this->dmi_normals.push_back(Neighbours::DMI_Normal_from_Pair(*geometry, dmi_pairs[ineigh], this->dmi_shell_chirality));
                this->dmi_magnitudes.push_back(dmi_shell_magnitudes[dmi_shells[ineigh]]);
            }
        }
        else
        {
            // Use direct list of pairs
            this->dmi_pairs      = this->dmi_pairs_in;
            this->dmi_magnitudes = this->dmi_magnitudes_in;
            this->dmi_normals    = this->dmi_normals_in;
            if( use_redundant_neighbours )
            {
                for (int i = 0; i < dmi_pairs_in.size(); ++i)
                {
                    auto& p = dmi_pairs_in[i];
                    auto& t = p.translations;
                    this->dmi_pairs.push_back(Pair{p.j, p.i, {-t[0], -t[1], -t[2]}});
                    this->dmi_magnitudes.push_back(dmi_magnitudes_in[i]);
                    this->dmi_normals.push_back(-dmi_normals_in[i]);
                }
            }
        }

        // Dipole-dipole (cutoff)
        scalar radius = this->ddi_cutoff_radius;
        if( this->ddi_method != DDI_Method::Cutoff )
            radius = 0;
        this->ddi_pairs      = Engine::Neighbours::Get_Pairs_in_Radius(*this->geometry, radius);
        this->ddi_magnitudes = scalarfield(this->ddi_pairs.size());
        this->ddi_normals    = vectorfield(this->ddi_pairs.size());

        for (unsigned int i = 0; i < this->ddi_pairs.size(); ++i)
        {
            Engine::Neighbours::DDI_from_Pair(
                *this->geometry,
                { this->ddi_pairs[i].i, this->ddi_pairs[i].j, this->ddi_pairs[i].translations },
                this->ddi_magnitudes[i], this->ddi_normals[i]);
        }
        // Dipole-dipole (FFT)
        if(ddi_method == DDI_Method::FFT)
            this->Prepare_DDI();

        // Update, which terms still contribute
        this->Update_Energy_Contributions();
    }

    void Hamiltonian_Heisenberg::Update_Energy_Contributions()
    {
        this->energy_contributions_per_spin = std::vector<std::pair<std::string, scalarfield>>(0);

        // External field
        if( this->external_field_magnitude > 0 )
        {
            this->energy_contributions_per_spin.push_back({"Zeeman", scalarfield(0)});
            this->idx_zeeman = this->energy_contributions_per_spin.size()-1;
        }
        else this->idx_zeeman = -1;
        // Anisotropy
        if( this->anisotropy_indices.size() > 0 )
        {
            this->energy_contributions_per_spin.push_back({"Anisotropy", scalarfield(0) });
            this->idx_anisotropy = this->energy_contributions_per_spin.size()-1;
        }
        else this->idx_anisotropy = -1;
        // Exchange
        if( this->exchange_pairs.size() > 0 )
        {
            this->energy_contributions_per_spin.push_back({"Exchange", scalarfield(0) });
            this->idx_exchange = this->energy_contributions_per_spin.size()-1;
        }
        else this->idx_exchange = -1;
        // DMI
        if( this->dmi_pairs.size() > 0 )
        {
            this->energy_contributions_per_spin.push_back({"DMI", scalarfield(0) });
            this->idx_dmi = this->energy_contributions_per_spin.size()-1;
        }
        else this->idx_dmi = -1;
        // Dipole-Dipole
        if( this->ddi_method != DDI_Method::None )
        {
            this->energy_contributions_per_spin.push_back({"DDI", scalarfield(0) });
            this->idx_ddi = this->energy_contributions_per_spin.size()-1;
        }
        else this->idx_ddi = -1;
       // Triplets
        if (this->triplets.size() > 0)
        {
            this->energy_contributions_per_spin.push_back({"triplets", scalarfield(0) });
            this->idx_triplet = this->energy_contributions_per_spin.size()-1;
        }
        else this->idx_triplet = -1;
        // Quadruplets
        if( this->quadruplets.size() > 0 )
        {
            this->energy_contributions_per_spin.push_back({"Quadruplets", scalarfield(0) });
            this->idx_quadruplet = this->energy_contributions_per_spin.size()-1;
        }
        else this->idx_quadruplet = -1;
    }

    void Hamiltonian_Heisenberg::Energy_Contributions_per_Spin(const vectorfield & spins, std::vector<std::pair<std::string, scalarfield>> & contributions)
    {
        if (contributions.size() != this->energy_contributions_per_spin.size())
        {
            contributions = this->energy_contributions_per_spin;
        }

        int nos = spins.size();
        for (auto& contrib : contributions)
        {
            // Allocate if not already allocated
            if (contrib.second.size() != nos) contrib.second = scalarfield(nos, 0);
            // Otherwise set to zero
            else Vectormath::fill(contrib.second, 0);
        }

        // External field
        if( this->idx_zeeman >=0 )     E_Zeeman(spins, contributions[idx_zeeman].second);

        // Anisotropy
        if( this->idx_anisotropy >=0 ) E_Anisotropy(spins, contributions[idx_anisotropy].second);

        // Exchange
        if( this->idx_exchange >=0 )   E_Exchange(spins, contributions[idx_exchange].second);
        // DMI
        if( this->idx_dmi >=0 )        E_DMI(spins,contributions[idx_dmi].second);
        // DDI
        if( this->idx_ddi >=0 )        E_DDI(spins, contributions[idx_ddi].second);
        // Triplets
        if (this->idx_triplet >=0 ) E_Triplet(spins, contributions[idx_triplet].second);
        // Quadruplets
        if (this->idx_quadruplet >=0 ) E_Quadruplet(spins, contributions[idx_quadruplet].second);
    }

    void Hamiltonian_Heisenberg::E_Zeeman(const vectorfield & spins, scalarfield & Energy)
    {
        const int N = geometry->n_cell_atoms;
        auto& mu_s = this->geometry->mu_s;

        #pragma omp parallel for
        for (int icell = 0; icell < geometry->n_cells_total; ++icell)
        {
            for (int ibasis = 0; ibasis < N; ++ibasis)
            {
                int ispin = icell*N + ibasis;
                if( check_atom_type(this->geometry->atom_types[ispin]) )
                    Energy[ispin] -= mu_s[ispin] * this->external_field_magnitude * this->external_field_normal.dot(spins[ispin]);
            }
        }
    }

    void Hamiltonian_Heisenberg::E_Anisotropy(const vectorfield & spins, scalarfield & Energy)
    {
        const int N = geometry->n_cell_atoms;

        #pragma omp parallel for
        for (int icell = 0; icell < geometry->n_cells_total; ++icell)
        {
            for (int iani = 0; iani < anisotropy_indices.size(); ++iani)
            {
                int ispin = icell*N + anisotropy_indices[iani];
                if (check_atom_type(this->geometry->atom_types[ispin]))
                    Energy[ispin] -= this->anisotropy_magnitudes[iani] * std::pow(anisotropy_normals[iani].dot(spins[ispin]), 2.0);
            }
        }
    }

    void Hamiltonian_Heisenberg::E_Exchange(const vectorfield & spins, scalarfield & Energy)
    {
        #pragma omp parallel for
        for (unsigned int icell = 0; icell < geometry->n_cells_total; ++icell)
        {
            for (unsigned int i_pair = 0; i_pair < exchange_pairs.size(); ++i_pair)
            {
                int ispin = exchange_pairs[i_pair].i + icell*geometry->n_cell_atoms;
                int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, exchange_pairs[i_pair]);
                if (jspin >= 0)
                {
                    Energy[ispin] -= 0.5 * exchange_magnitudes[i_pair] * spins[ispin].dot(spins[jspin]);
                    #ifndef _OPENMP
                    Energy[jspin] -= 0.5 * exchange_magnitudes[i_pair] * spins[ispin].dot(spins[jspin]);
                    #endif
                }
            }
        }
    }

    void Hamiltonian_Heisenberg::E_DMI(const vectorfield & spins, scalarfield & Energy)
    {
        #pragma omp parallel for
        for (unsigned int icell = 0; icell < geometry->n_cells_total; ++icell)
        {
            for (unsigned int i_pair = 0; i_pair < dmi_pairs.size(); ++i_pair)
            {
                int ispin = dmi_pairs[i_pair].i + icell*geometry->n_cell_atoms;
                int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, dmi_pairs[i_pair]);
                if (jspin >= 0)
                {
                    Energy[ispin] -= 0.5 * dmi_magnitudes[i_pair] * dmi_normals[i_pair].dot(spins[ispin].cross(spins[jspin]));
                    #ifndef _OPENMP
                    Energy[jspin] -= 0.5 * dmi_magnitudes[i_pair] * dmi_normals[i_pair].dot(spins[ispin].cross(spins[jspin]));
                    #endif
                }
            }
        }
    }

    void Hamiltonian_Heisenberg::E_DDI(const vectorfield & spins, scalarfield & Energy)
    {
        if( this->ddi_method == DDI_Method::FFT )
            this->E_DDI_FFT(spins, Energy);
        else if( this->ddi_method == DDI_Method::Cutoff )
            // TODO: Merge these implementations in the future
            if(ddi_cutoff_radius >= 0)
                this->E_DDI_Cutoff(spins, Energy);
            else 
                this->E_DDI_Direct(spins, Energy);
    }

    void Hamiltonian_Heisenberg::E_DDI_Direct(const vectorfield & spins, scalarfield & Energy)
    {
        vectorfield gradients_temp;
        gradients_temp.resize(geometry->nos);
        Vectormath::fill(gradients_temp, {0,0,0});
        this->Gradient_DDI_Direct(spins, gradients_temp);

        #pragma omp parallel for
        for (int ispin = 0; ispin < geometry->nos; ispin++)
        {
            Energy[ispin] += 0.5 * geometry->mu_s[ispin] * spins[ispin].dot(gradients_temp[ispin]);
        }   
    }

    void Hamiltonian_Heisenberg::E_DDI_Cutoff(const vectorfield & spins, scalarfield & Energy)
    {
        auto& mu_s = this->geometry->mu_s;
        // The translations are in angstr�m, so the |r|[m] becomes |r|[m]*10^-10
        const scalar mult = C::mu_0 * std::pow(C::mu_B, 2) / ( 4*C::Pi * 1e-30 );

        scalar result = 0.0;

        for (unsigned int i_pair = 0; i_pair < ddi_pairs.size(); ++i_pair)
        {
            if (ddi_magnitudes[i_pair] > 0.0)
            {
                for (int da = 0; da < geometry->n_cells[0]; ++da)
                {
                    for (int db = 0; db < geometry->n_cells[1]; ++db)
                    {
                        for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                        {
                            std::array<int, 3 > translations = { da, db, dc };
                            int i = ddi_pairs[i_pair].i;
                            int j = ddi_pairs[i_pair].j;
                            int ispin = i + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                            int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, ddi_pairs[i_pair]);
                            if (jspin >= 0)
                            {
                                Energy[ispin] -= 0.5 * mu_s[ispin] * mu_s[jspin] * mult / std::pow(ddi_magnitudes[i_pair], 3.0) *
                                    (3 * spins[ispin].dot(ddi_normals[i_pair]) * spins[ispin].dot(ddi_normals[i_pair]) - spins[ispin].dot(spins[ispin]));
                                Energy[jspin] -= 0.5 * mu_s[ispin] * mu_s[jspin] * mult / std::pow(ddi_magnitudes[i_pair], 3.0) *
                                    (3 * spins[ispin].dot(ddi_normals[i_pair]) * spins[ispin].dot(ddi_normals[i_pair]) - spins[ispin].dot(spins[ispin]));
                            }
                        }
                    }
                }
            }
        }
    }// end DipoleDipole


    void Hamiltonian_Heisenberg::E_DDI_FFT(const vectorfield & spins, scalarfield & Energy)
    {
        scalar Energy_DDI = 0;
        vectorfield gradients_temp;
        gradients_temp.resize(geometry->nos);
        Vectormath::fill(gradients_temp, {0,0,0});
        this->Gradient_DDI_FFT(spins, gradients_temp);

        // === DEBUG: begin gradient comparison ===
            // vectorfield gradients_temp_dir;
            // gradients_temp_dir.resize(this->geometry->nos);
            // Vectormath::fill(gradients_temp_dir, {0,0,0});
            // Gradient_DDI_Direct(spins, gradients_temp_dir);

            // //get deviation
            // std::array<scalar, 3> deviation = {0,0,0};
            // std::array<scalar, 3> avg = {0,0,0};
            // for(int i = 0; i < this->geometry->nos; i++)
            // {
            //     for(int d = 0; d < 3; d++)
            //     {
            //         deviation[d] += std::pow(gradients_temp[i][d] - gradients_temp_dir[i][d], 2);
            //         avg[d] += gradients_temp_dir[i][d];
            //     }
            // }
            // std::cerr << "Avg. Gradient = " << avg[0]/this->geometry->nos << " " << avg[1]/this->geometry->nos << " " << avg[2]/this->geometry->nos << std::endl;
            // std::cerr << "Avg. Deviation = " << deviation[0]/this->geometry->nos << " " << deviation[1]/this->geometry->nos << " " << deviation[2]/this->geometry->nos << std::endl;
        //==== DEBUG: end gradient comparison ====

        // TODO: add dot_scaled to Vectormath and use that
        #pragma omp parallel for
        for (int ispin = 0; ispin < geometry->nos; ispin++)
        {
            Energy[ispin] += 0.5 * geometry->mu_s[ispin] * spins[ispin].dot(gradients_temp[ispin]);
            Energy_DDI    += 0.5 * geometry->mu_s[ispin] * spins[ispin].dot(gradients_temp[ispin]);
        }
    }

void Hamiltonian_Heisenberg::E_Triplet(const vectorfield & spins, scalarfield & Energy)
    {
        for (unsigned int itrip = 0; itrip < triplets.size(); ++itrip)
        {
            for (int da = 0; da < geometry->n_cells[0]; ++da)
            {
                for (int db = 0; db < geometry->n_cells[1]; ++db)
                {
                    for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                    {
                        std::array<int, 3 > translations = { da, db, dc };
                        int ispin = triplets[itrip].i + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                        int jspin = triplets[itrip].j + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, triplets[itrip].d_j);
                        int kspin = triplets[itrip].k + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, triplets[itrip].d_k);
                        Vector3 n = {triplets[itrip].n[0], triplets[itrip].n[1], triplets[itrip].n[2]};
                        if ( check_atom_type(this->geometry->atom_types[ispin]) && check_atom_type(this->geometry->atom_types[jspin]) &&
                                check_atom_type(this->geometry->atom_types[kspin]))
                        {
                            Energy[ispin] -= 1.0/3.0 * triplet_magnitudes1[itrip] * pow(spins[ispin].dot(spins[jspin].cross(spins[kspin])),2);
                            Energy[jspin] -= 1.0/3.0 * triplet_magnitudes1[itrip] * pow(spins[ispin].dot(spins[jspin].cross(spins[kspin])),2);
                            Energy[kspin] -= 1.0/3.0 * triplet_magnitudes1[itrip] * pow(spins[ispin].dot(spins[jspin].cross(spins[kspin])),2);
                            Energy[ispin] -= 1.0/3.0 * triplet_magnitudes2[itrip] * spins[ispin].dot(spins[jspin].cross(spins[kspin]))
                                                * (n.dot(spins[ispin]+spins[jspin]+spins[kspin]));                               
                            Energy[jspin] -= 1.0/3.0 * triplet_magnitudes2[itrip] * spins[ispin].dot(spins[jspin].cross(spins[kspin]))
                                                * (n.dot(spins[ispin]+spins[jspin]+spins[kspin]));
                            Energy[kspin] -= 1.0/3.0 * triplet_magnitudes2[itrip] * spins[ispin].dot(spins[jspin].cross(spins[kspin]))
                                                * (n.dot(spins[ispin]+spins[jspin]+spins[kspin]));
                        }
                    }
                }
            }
        }
    }

    void Hamiltonian_Heisenberg::E_Quadruplet(const vectorfield & spins, scalarfield & Energy)
    {
        for (unsigned int iquad = 0; iquad < quadruplets.size(); ++iquad)
        {
            for (int da = 0; da < geometry->n_cells[0]; ++da)
            {
                for (int db = 0; db < geometry->n_cells[1]; ++db)
                {
                    for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                    {
                        std::array<int, 3 > translations = { da, db, dc };
                        int ispin = quadruplets[iquad].i + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                        int jspin = quadruplets[iquad].j + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_j);
                        int kspin = quadruplets[iquad].k + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_k);
                        int lspin = quadruplets[iquad].l + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_l);

                        if ( check_atom_type(this->geometry->atom_types[ispin]) && check_atom_type(this->geometry->atom_types[jspin]) &&
                                check_atom_type(this->geometry->atom_types[kspin]) && check_atom_type(this->geometry->atom_types[lspin]) )
                        {
                            Energy[ispin] -= 0.25*quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * (spins[kspin].dot(spins[lspin]));
                            Energy[jspin] -= 0.25*quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * (spins[kspin].dot(spins[lspin]));
                            Energy[kspin] -= 0.25*quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * (spins[kspin].dot(spins[lspin]));
                            Energy[lspin] -= 0.25*quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * (spins[kspin].dot(spins[lspin]));
                        }
                    }
                }
            }
        }
    }


    scalar Hamiltonian_Heisenberg::Energy_Single_Spin(int ispin_in, const vectorfield & spins)
    {
        scalar Energy = 0;
        if( check_atom_type(this->geometry->atom_types[ispin_in]) )
        {
            int icell  = ispin_in / this->geometry->n_cell_atoms;
            int ibasis = ispin_in - icell*this->geometry->n_cell_atoms;
            auto& mu_s = this->geometry->mu_s;

            // External field
            if (this->idx_zeeman >= 0)
                Energy -= mu_s[ispin_in] * this->external_field_magnitude * this->external_field_normal.dot(spins[ispin_in]);

            // Anisotropy
            if (this->idx_anisotropy >= 0)
            {
                for (int iani = 0; iani < anisotropy_indices.size(); ++iani)
                {
                    if (anisotropy_indices[iani] == ibasis)
                    {
                        if (check_atom_type(this->geometry->atom_types[ispin_in]))
                            Energy -= this->anisotropy_magnitudes[iani] * std::pow(anisotropy_normals[iani].dot(spins[ispin_in]), 2.0);
                    }
                }
            }

            // Exchange
            if (this->idx_exchange >= 0)
            {
                for (unsigned int ipair = 0; ipair < exchange_pairs.size(); ++ipair)
                {
                    if (exchange_pairs[ipair].i == ibasis)
                    {
                        int ispin = exchange_pairs[ipair].i + icell*geometry->n_cell_atoms;
                        int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, exchange_pairs[ipair]);
                        if (jspin >= 0)
                        {
                            Energy -= 0.5 * this->exchange_magnitudes[ipair] * spins[ispin].dot(spins[jspin]);
                        }
                        #ifndef _OPENMP
                        jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, exchange_pairs[ipair], true);
                        if (jspin >= 0)
                        {
                            Energy -= 0.5 * this->exchange_magnitudes[ipair] * spins[ispin].dot(spins[jspin]);
                        }
                        #endif
                    }
                }
            }

            // DMI
            if (this->idx_dmi >= 0)
            {
                for (unsigned int ipair = 0; ipair < dmi_pairs.size(); ++ipair)
                {
                    if (dmi_pairs[ipair].i == ibasis)
                    {
                        int ispin = dmi_pairs[ipair].i + icell*geometry->n_cell_atoms;
                        int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, dmi_pairs[ipair]);
                        if (jspin >= 0)
                        {
                            Energy -= 0.5 * this->dmi_magnitudes[ipair] * this->dmi_normals[ipair].dot(spins[ispin].cross(spins[jspin]));
                        }
                        #ifndef _OPENMP
                        jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, dmi_pairs[ipair], true);
                        if (jspin >= 0)
                        {
                            Energy += 0.5 * this->dmi_magnitudes[ipair] * this->dmi_normals[ipair].dot(spins[ispin].cross(spins[jspin]));
                        }
                        #endif
                    }
                }
            }

            // DDI
            if (this->idx_ddi >= 0)
            {
                for (unsigned int ipair = 0; ipair < ddi_pairs.size(); ++ipair)
                {
                    if (ddi_pairs[ipair].i == ibasis)
                    {
                        int ispin = ddi_pairs[ipair].i + icell*geometry->n_cell_atoms;
                        int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, ddi_pairs[ipair]);

                        // The translations are in angstr�m, so the |r|[m] becomes |r|[m]*10^-10
                        const scalar mult = 0.5 * mu_s[ispin] * mu_s[jspin]
                            * C::mu_0 * std::pow(C::mu_B, 2) / ( 4*C::Pi * 1e-30 );

                        if (jspin >= 0)
                        {
                            Energy -= mult / std::pow(this->ddi_magnitudes[ipair], 3.0) *
                                (3 * spins[ispin].dot(this->ddi_normals[ipair]) * spins[ispin].dot(this->ddi_normals[ipair]) - spins[ispin].dot(spins[ispin]));

                        }
                        #ifndef _OPENMP
                        jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, dmi_pairs[ipair], true);
                        if (jspin >= 0)
                        {
                            Energy += mult / std::pow(this->ddi_magnitudes[ipair], 3.0) *
                                (3 * spins[ispin].dot(this->ddi_normals[ipair]) * spins[ispin].dot(this->ddi_normals[ipair]) - spins[ispin].dot(spins[ispin]));
                        }
                        #endif
                    }
                }
            }

            // Triplets
            if (this->idx_triplet >= 0)
            {

                for (unsigned int itrip = 0; itrip < quadruplets.size(); ++itrip)
                {
                    auto translations = Vectormath::translations_from_idx(geometry->n_cells, geometry->n_cell_atoms, icell);
                    int ispin = quadruplets[itrip].i + icell*geometry->n_cell_atoms;
                    int jspin = quadruplets[itrip].j + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[itrip].d_j);
                    int kspin = quadruplets[itrip].k + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[itrip].d_k);
                    Vector3 n = {triplets[itrip].n[0], triplets[itrip].n[1], triplets[itrip].n[2]};

                    if ( check_atom_type(this->geometry->atom_types[ispin]) && check_atom_type(this->geometry->atom_types[jspin]) &&
                        check_atom_type(this->geometry->atom_types[kspin]))
                    {
                            Energy -= 1.0/3.0 * triplet_magnitudes1[itrip] * pow(spins[ispin].dot(spins[jspin].cross(spins[kspin])),2);
                            Energy -= 1.0/3.0 * triplet_magnitudes2[itrip] * spins[ispin].dot(spins[jspin].cross(spins[kspin]))
                                                * (n.dot(spins[ispin]+spins[jspin]+spins[kspin]));                               
                    }

                    #ifndef _OPENMP
                    // TODO: mirrored quadruplet when unique quadruplets are used
                    // jspin = quadruplets[iquad].j + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_j, true);
                    // kspin = quadruplets[iquad].k + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_k, true);
                    // lspin = quadruplets[iquad].l + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_l, true);

                    // if ( check_atom_type(this->geometry->atom_types[ispin]) && check_atom_type(this->geometry->atom_types[jspin]) &&
                    //      check_atom_type(this->geometry->atom_types[kspin]) && check_atom_type(this->geometry->atom_types[lspin]) )
                    // {
                    //     Energy -= 0.25*quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * (spins[kspin].dot(spins[lspin]));
                    // }
                    #endif
                }
            }

            // Quadruplets
            if (this->idx_quadruplet >= 0)
            {
                for (unsigned int iquad = 0; iquad < quadruplets.size(); ++iquad)
                {
                    auto translations = Vectormath::translations_from_idx(geometry->n_cells, geometry->n_cell_atoms, icell);
                    int ispin = quadruplets[iquad].i + icell*geometry->n_cell_atoms;
                    int jspin = quadruplets[iquad].j + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_j);
                    int kspin = quadruplets[iquad].k + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_k);
                    int lspin = quadruplets[iquad].l + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_l);

                    if ( check_atom_type(this->geometry->atom_types[ispin]) && check_atom_type(this->geometry->atom_types[jspin]) &&
                        check_atom_type(this->geometry->atom_types[kspin]) && check_atom_type(this->geometry->atom_types[lspin]) )
                    {
                        Energy -= 0.25*quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * (spins[kspin].dot(spins[lspin]));
                    }

                    #ifndef _OPENMP
                    // TODO: mirrored quadruplet when unique quadruplets are used
                    // jspin = quadruplets[iquad].j + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_j, true);
                    // kspin = quadruplets[iquad].k + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_k, true);
                    // lspin = quadruplets[iquad].l + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_l, true);

                    // if ( check_atom_type(this->geometry->atom_types[ispin]) && check_atom_type(this->geometry->atom_types[jspin]) &&
                    //      check_atom_type(this->geometry->atom_types[kspin]) && check_atom_type(this->geometry->atom_types[lspin]) )
                    // {
                    //     Energy -= 0.25*quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * (spins[kspin].dot(spins[lspin]));
                    // }
                    #endif
                }
            }
        }
        return Energy;
    }


    void Hamiltonian_Heisenberg::Gradient(const vectorfield & spins, vectorfield & gradient)
    {
        // Set to zero
        Vectormath::fill(gradient, {0,0,0});

        // External field
        this->Gradient_Zeeman(gradient);

        // Anisotropy
        this->Gradient_Anisotropy(spins, gradient);

        // Exchange
        this->Gradient_Exchange(spins, gradient);

        // DMI
        this->Gradient_DMI(spins, gradient);

        // DD
        this->Gradient_DDI(spins, gradient);

        // Triplets
        this->Gradient_Triplet(spins, gradient);

        // Quadruplets
        this->Gradient_Quadruplet(spins, gradient);
    }

    void Hamiltonian_Heisenberg::Gradient_Zeeman(vectorfield & gradient)
    {
        const int N = geometry->n_cell_atoms;
        auto& mu_s = this->geometry->mu_s;

        #pragma omp parallel for
        for (int icell = 0; icell < geometry->n_cells_total; ++icell)
        {
            for (int ibasis = 0; ibasis < N; ++ibasis)
            {
                int ispin = icell*N + ibasis;
                if (check_atom_type(this->geometry->atom_types[ispin]))
                    gradient[ispin] -= mu_s[ispin] * this->external_field_magnitude * this->external_field_normal;
            }
        }
    }

    void Hamiltonian_Heisenberg::Gradient_Anisotropy(const vectorfield & spins, vectorfield & gradient)
    {
        const int N = geometry->n_cell_atoms;

        #pragma omp parallel for
        for (int icell = 0; icell < geometry->n_cells_total; ++icell)
        {
            for (int iani = 0; iani < anisotropy_indices.size(); ++iani)
            {
                int ispin = icell*N + anisotropy_indices[iani];
                if (check_atom_type(this->geometry->atom_types[ispin]))
                    gradient[ispin] -= 2.0 * this->anisotropy_magnitudes[iani] * this->anisotropy_normals[iani] * anisotropy_normals[iani].dot(spins[ispin]);
            }
        }
    }

    void Hamiltonian_Heisenberg::Gradient_Exchange(const vectorfield & spins, vectorfield & gradient)
    {
        #pragma omp parallel for
        for (int icell = 0; icell < geometry->n_cells_total; ++icell)
        {
            for (unsigned int i_pair = 0; i_pair < exchange_pairs.size(); ++i_pair)
            {
                int ispin = exchange_pairs[i_pair].i + icell*geometry->n_cell_atoms;
                int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, exchange_pairs[i_pair]);
                if (jspin >= 0)
                {
                    gradient[ispin] -= exchange_magnitudes[i_pair] * spins[jspin];
                    #ifndef _OPENMP
                    gradient[jspin] -= exchange_magnitudes[i_pair] * spins[ispin];
                    #endif
                }
            }
        }
    }

    void Hamiltonian_Heisenberg::Gradient_DMI(const vectorfield & spins, vectorfield & gradient)
    {
        #pragma omp parallel for
        for (int icell = 0; icell < geometry->n_cells_total; ++icell)
        {
            for (unsigned int i_pair = 0; i_pair < dmi_pairs.size(); ++i_pair)
            {
                int ispin = dmi_pairs[i_pair].i + icell*geometry->n_cell_atoms;
                int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, dmi_pairs[i_pair]);
                if (jspin >= 0)
                {
                    gradient[ispin] -= dmi_magnitudes[i_pair] * spins[jspin].cross(dmi_normals[i_pair]);
                    #ifndef _OPENMP
                    gradient[jspin] += dmi_magnitudes[i_pair] * spins[ispin].cross(dmi_normals[i_pair]);
                    #endif
                }
            }
        }
    }

    void Hamiltonian_Heisenberg::Gradient_DDI(const vectorfield & spins, vectorfield & gradient)
    {
        if( this->ddi_method == DDI_Method::FFT )
            this->Gradient_DDI_FFT(spins, gradient);
        else if( this->ddi_method == DDI_Method::Cutoff )
            // TODO: Merge these implementations in the future
            if( this->ddi_cutoff_radius >= 0)
                this->Gradient_DDI_Cutoff(spins, gradient);
            else
                this->Gradient_DDI_Direct(spins, gradient);
    }

    void Hamiltonian_Heisenberg::Gradient_DDI_Cutoff(const vectorfield & spins, vectorfield & gradient)
    {
        auto& mu_s = this->geometry->mu_s;
        // The translations are in angstr�m, so the |r|[m] becomes |r|[m]*10^-10
        const scalar mult = C::mu_0 * C::mu_B * C::mu_B / ( 4*C::Pi * 1e-30 );

        for (unsigned int i_pair = 0; i_pair < ddi_pairs.size(); ++i_pair)
        {
            if (ddi_magnitudes[i_pair] > 0.0)
            {
                for (int da = 0; da < geometry->n_cells[0]; ++da)
                {
                    for (int db = 0; db < geometry->n_cells[1]; ++db)
                    {
                        for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                        {
                            scalar skalar_contrib = mult / std::pow(ddi_magnitudes[i_pair], 3.0);
                            std::array<int, 3 > translations = { da, db, dc };

                            int i = ddi_pairs[i_pair].i;
                            int j = ddi_pairs[i_pair].j;
                            int ispin = i + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                            int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, ddi_pairs[i_pair]);
                            if (jspin >= 0)
                            {
                                gradient[ispin] -= mu_s[jspin] * skalar_contrib * (3 * ddi_normals[i_pair] * spins[jspin].dot(ddi_normals[i_pair]) - spins[jspin]);
                                gradient[jspin] -= mu_s[ispin] * skalar_contrib * (3 * ddi_normals[i_pair] * spins[ispin].dot(ddi_normals[i_pair]) - spins[ispin]);
                            }
                        }
                    }
                }
            }
        }
    }//end Field_DipoleDipole

    void Hamiltonian_Heisenberg::Gradient_DDI_FFT(const vectorfield & spins, vectorfield & gradient)
    {
        // Size of original geometry
        int Na = geometry->n_cells[0];
        int Nb = geometry->n_cells[1];
        int Nc = geometry->n_cells[2];

        FFT_Spins(spins);

        auto& ft_D_matrices = fft_plan_dipole.cpx_ptr;
        auto& ft_spins = fft_plan_spins.cpx_ptr;

        auto& res_iFFT = fft_plan_reverse.real_ptr;
        auto& res_mult = fft_plan_reverse.cpx_ptr;

        int idx_b1, idx_b2, idx_d;

        // Loop over basis atoms (i.e sublattices)
        #pragma omp parallel for collapse(5)
        for(int i_b1 = 0; i_b1 < geometry->n_cell_atoms; ++i_b1)
        {
            for(int c = 0; c < it_bounds_pointwise_mult[2]; ++c)
            {
                for(int b = 0; b < it_bounds_pointwise_mult[1]; ++b)
                {
                    for(int a = 0; a < it_bounds_pointwise_mult[0]; ++a)
                    {
                        for(int i_b2 = 0; i_b2 < geometry->n_cell_atoms; ++i_b2)
                        {
                            // Look up at which position the correct D-matrices are saved
                            int& b_inter = inter_sublattice_lookup[i_b1 + i_b2 * geometry->n_cell_atoms];

                            idx_b2 = i_b2 * spin_stride.basis + a * spin_stride.a + b * spin_stride.b + c * spin_stride.c;
                            idx_b1 = i_b1 * spin_stride.basis + a * spin_stride.a + b * spin_stride.b + c * spin_stride.c;
                            idx_d  = b_inter * dipole_stride.basis + a * dipole_stride.a + b * dipole_stride.b + c * dipole_stride.c;

                            auto& fs_x = ft_spins[idx_b2                       ];
                            auto& fs_y = ft_spins[idx_b2 + 1 * spin_stride.comp];
                            auto& fs_z = ft_spins[idx_b2 + 2 * spin_stride.comp];

                            auto& fD_xx = ft_D_matrices[idx_d                    ];
                            auto& fD_xy = ft_D_matrices[idx_d + 1 * dipole_stride.comp];
                            auto& fD_xz = ft_D_matrices[idx_d + 2 * dipole_stride.comp];
                            auto& fD_yy = ft_D_matrices[idx_d + 3 * dipole_stride.comp];
                            auto& fD_yz = ft_D_matrices[idx_d + 4 * dipole_stride.comp];
                            auto& fD_zz = ft_D_matrices[idx_d + 5 * dipole_stride.comp];

                            FFT::addTo(res_mult[idx_b1 + 0 * spin_stride.comp], FFT::mult3D(fD_xx, fD_xy, fD_xz, fs_x, fs_y, fs_z), i_b2 == 0);
                            FFT::addTo(res_mult[idx_b1 + 1 * spin_stride.comp], FFT::mult3D(fD_xy, fD_yy, fD_yz, fs_x, fs_y, fs_z), i_b2 == 0);
                            FFT::addTo(res_mult[idx_b1 + 2 * spin_stride.comp], FFT::mult3D(fD_xz, fD_yz, fD_zz, fs_x, fs_y, fs_z), i_b2 == 0);
                        }
                    }
                }//end iteration over padded lattice cells
            }//end iteration over second sublattice
        }

        //Inverse Fourier Transform
        FFT::batch_iFour_3D(fft_plan_reverse);

        #pragma omp parallel for collapse(4)
        //Place the gradients at the correct positions and mult with correct mu
        for(int c = 0; c < geometry->n_cells[2]; ++c)
        {
            for(int b = 0; b < geometry->n_cells[1]; ++b)
            {
                for(int a = 0; a < geometry->n_cells[0]; ++a)
                {
                    for(int i_b1 = 0; i_b1 < geometry->n_cell_atoms; ++i_b1)
                    {
                        int idx_orig = i_b1 + geometry->n_cell_atoms * (a + Na * (b + Nb * c));
                        int idx = i_b1 * spin_stride.basis + a * spin_stride.a + b * spin_stride.b + c * spin_stride.c;
                        gradient[idx_orig][0] -= res_iFFT[idx                       ] / sublattice_size;
                        gradient[idx_orig][1] -= res_iFFT[idx + 1 * spin_stride.comp] / sublattice_size;
                        gradient[idx_orig][2] -= res_iFFT[idx + 2 * spin_stride.comp] / sublattice_size;
                    }
                }
            }
        }//end iteration sublattice 1
    }

    void Hamiltonian_Heisenberg::Gradient_DDI_Direct(const vectorfield & spins, vectorfield & gradient)
    {
        scalar mult = C::mu_0 * C::mu_B * C::mu_B / ( 4*C::Pi * 1e-30 );
        scalar d, d3, d5;
        Vector3 diff;
        Vector3 diff_img;

        int img_a = boundary_conditions[0] == 0 ? 0 : ddi_n_periodic_images[0];
        int img_b = boundary_conditions[1] == 0 ? 0 : ddi_n_periodic_images[1];
        int img_c = boundary_conditions[2] == 0 ? 0 : ddi_n_periodic_images[2];

        for(int idx1 = 0; idx1 < geometry->nos; idx1++)
        {
            for(int idx2 = 0; idx2 < geometry->nos; idx2++)
            {
                auto& m2 = spins[idx2];

                diff = geometry->lattice_constant * (this->geometry->positions[idx2] - this->geometry->positions[idx1]);
                scalar Dxx = 0, Dxy = 0, Dxz = 0, Dyy = 0, Dyz = 0, Dzz = 0;

                for(int a_pb = - img_a; a_pb <= img_a; a_pb++)
                {
                    for(int b_pb = - img_b; b_pb <= img_b; b_pb++)
                    {
                        for(int c_pb = -img_c; c_pb <= img_c; c_pb++)
                        {
                            diff_img = diff + geometry->lattice_constant * a_pb * geometry->n_cells[0] * geometry->bravais_vectors[0]
                                            + geometry->lattice_constant * b_pb * geometry->n_cells[1] * geometry->bravais_vectors[1]
                                            + geometry->lattice_constant * c_pb * geometry->n_cells[2] * geometry->bravais_vectors[2];
                            d = diff_img.norm();
                            if(d > 1e-10)
                            {
                                d3 = d * d * d;
                                d5 = d * d * d * d * d;
                                Dxx += mult * (3 * diff_img[0]*diff_img[0] / d5 - 1/d3);
                                Dxy += mult *  3 * diff_img[0]*diff_img[1] / d5;          //same as Dyx
                                Dxz += mult *  3 * diff_img[0]*diff_img[2] / d5;          //same as Dzx
                                Dyy += mult * (3 * diff_img[1]*diff_img[1] / d5 - 1/d3);
                                Dyz += mult *  3 * diff_img[1]*diff_img[2] / d5;          //same as Dzy
                                Dzz += mult * (3 * diff_img[2]*diff_img[2] / d5 - 1/d3);
                            }
                        }
                    }
                }

                auto& mu = geometry->mu_s[idx2];

                gradient[idx1][0] -= (Dxx * m2[0] + Dxy * m2[1] + Dxz * m2[2]) * mu;
                gradient[idx1][1] -= (Dxy * m2[0] + Dyy * m2[1] + Dyz * m2[2]) * mu;
                gradient[idx1][2] -= (Dxz * m2[0] + Dyz * m2[1] + Dzz * m2[2]) * mu;
            }
        }
    }

void Hamiltonian_Heisenberg::Gradient_Triplet(const vectorfield & spins, vectorfield & gradient)
    {
       for (unsigned int itrip = 0; itrip < triplets.size(); ++itrip)
        {
            int i = triplets[itrip].i;
            int j = triplets[itrip].j;
            int k = triplets[itrip].k;
            Vector3 n = {triplets[itrip].n[0], triplets[itrip].n[1], triplets[itrip].n[2]};
            for (int da = 0; da < geometry->n_cells[0]; ++da)
            {
                for (int db = 0; db < geometry->n_cells[1]; ++db)
                {
                    for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                    {
                        std::array<int, 3 > translations = { da, db, dc };
                        int ispin = i + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                        int jspin = j + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, triplets[itrip].d_j);
                        int kspin = k + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, triplets[itrip].d_k);
                        
                        if ( check_atom_type(this->geometry->atom_types[ispin]) && check_atom_type(this->geometry->atom_types[jspin]) &&
                                check_atom_type(this->geometry->atom_types[kspin]))
                        {
                            gradient[ispin] -= 2.0 * triplet_magnitudes1[itrip] * spins[ispin].dot(spins[jspin].cross(spins[kspin]))
                                                * spins[jspin].cross(spins[kspin]);
                            gradient[jspin] -= 2.0 * triplet_magnitudes1[itrip] * spins[ispin].dot(spins[jspin].cross(spins[kspin]))
                                                * spins[kspin].cross(spins[ispin]);
                            gradient[kspin] -= 2.0 * triplet_magnitudes1[itrip] * spins[ispin].dot(spins[jspin].cross(spins[kspin]))
                                                * spins[ispin].cross(spins[jspin]);
                             
                            gradient[ispin] -= triplet_magnitudes2[itrip] * (n.dot(spins[ispin]+spins[jspin]+spins[kspin])
                                                * spins[jspin].cross(spins[kspin])
                                               + spins[ispin].dot(spins[jspin].cross(spins[kspin])) * n);
                            gradient[jspin] -= triplet_magnitudes2[itrip] * (n.dot(spins[ispin]+spins[jspin]+spins[kspin])
                                                * spins[kspin].cross(spins[ispin])
                                               + spins[ispin].dot(spins[jspin].cross(spins[kspin])) * n);
                            gradient[kspin] -= triplet_magnitudes2[itrip] * (n.dot(spins[ispin]+spins[jspin]+spins[kspin])
                                                * spins[ispin].cross(spins[jspin])
                                               + spins[ispin].dot(spins[jspin].cross(spins[kspin])) * n);
                            std::cout << gradient[ispin] << "  \n" << gradient[jspin] << "  \n" << gradient[kspin] << "\n" << std::endl;
                        }
                    }
                }
            }
        }
    }

    void Hamiltonian_Heisenberg::Gradient_Quadruplet(const vectorfield & spins, vectorfield & gradient)
    {
        for (unsigned int iquad = 0; iquad < quadruplets.size(); ++iquad)
        {
            int i = quadruplets[iquad].i;
            int j = quadruplets[iquad].j;
            int k = quadruplets[iquad].k;
            int l = quadruplets[iquad].l;
            for (int da = 0; da < geometry->n_cells[0]; ++da)
            {
                for (int db = 0; db < geometry->n_cells[1]; ++db)
                {
                    for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                    {
                        std::array<int, 3 > translations = { da, db, dc };
                        int ispin = i + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                        int jspin = j + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_j);
                        int kspin = k + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_k);
                        int lspin = l + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations, quadruplets[iquad].d_l);

                        if ( check_atom_type(this->geometry->atom_types[ispin]) && check_atom_type(this->geometry->atom_types[jspin]) &&
                                check_atom_type(this->geometry->atom_types[kspin]) && check_atom_type(this->geometry->atom_types[lspin]) )
                        {
                            gradient[ispin] -= quadruplet_magnitudes[iquad] * spins[jspin] * (spins[kspin].dot(spins[lspin]));
                            gradient[jspin] -= quadruplet_magnitudes[iquad] * spins[ispin] * (spins[kspin].dot(spins[lspin]));
                            gradient[kspin] -= quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * spins[lspin];
                            gradient[lspin] -= quadruplet_magnitudes[iquad] * (spins[ispin].dot(spins[jspin])) * spins[kspin];
                        }
                    }
                }
            }
        }
    }


    void Hamiltonian_Heisenberg::Hessian(const vectorfield & spins, MatrixX & hessian)
    {
        int nos = spins.size();

        // Set to zero
        hessian.setZero();

        // Single Spin elements
        for (int da = 0; da < geometry->n_cells[0]; ++da)
        {
            for (int db = 0; db < geometry->n_cells[1]; ++db)
            {
                for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                {
                    std::array<int, 3 > translations = { da, db, dc };
                    int icell = Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                    for (int alpha = 0; alpha < 3; ++alpha)
                    {
                        for ( int beta = 0; beta < 3; ++beta )
                        {
                            for (unsigned int i = 0; i < anisotropy_indices.size(); ++i)
                            {
                                if ( check_atom_type(this->geometry->atom_types[icell+anisotropy_indices[i]]) )
                                {
                                    int idx_i = 3 * icell + anisotropy_indices[i] + alpha;
                                    int idx_j = 3 * icell + anisotropy_indices[i] + beta;
                                    // scalar x = -2.0*this->anisotropy_magnitudes[i] * std::pow(this->anisotropy_normals[i][alpha], 2);
                                    hessian( idx_i, idx_j ) += -2.0 * this->anisotropy_magnitudes[i] *
                                                                    this->anisotropy_normals[i][alpha] *
                                                                    this->anisotropy_normals[i][beta];
                                }
                            }
                        }
                    }
                }
            }
        }

        // Spin Pair elements
        // Exchange
        for (int da = 0; da < geometry->n_cells[0]; ++da)
        {
            for (int db = 0; db < geometry->n_cells[1]; ++db)
            {
                for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                {
                    std::array<int, 3 > translations = { da, db, dc };
                    for (unsigned int i_pair = 0; i_pair < this->exchange_pairs.size(); ++i_pair)
                    {
                        int ispin = exchange_pairs[i_pair].i + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                        int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, exchange_pairs[i_pair]);
                        if (jspin >= 0)
                        {
                            for (int alpha = 0; alpha < 3; ++alpha)
                            {
                                int i = 3 * ispin + alpha;
                                int j = 3 * jspin + alpha;

                                hessian(i, j) += -exchange_magnitudes[i_pair];
                                #ifndef _OPENMP
                                hessian(j, i) += -exchange_magnitudes[i_pair];
                                #endif
                            }
                        }
                    }
                }
            }
        }

        // DMI
        for (int da = 0; da < geometry->n_cells[0]; ++da)
        {
            for (int db = 0; db < geometry->n_cells[1]; ++db)
            {
                for (int dc = 0; dc < geometry->n_cells[2]; ++dc)
                {
                    std::array<int, 3 > translations = { da, db, dc };
                    for (unsigned int i_pair = 0; i_pair < this->dmi_pairs.size(); ++i_pair)
                    {
                        int ispin = dmi_pairs[i_pair].i + Vectormath::idx_from_translations(geometry->n_cells, geometry->n_cell_atoms, translations);
                        int jspin = idx_from_pair(ispin, boundary_conditions, geometry->n_cells, geometry->n_cell_atoms, geometry->atom_types, dmi_pairs[i_pair]);
                        if (jspin >= 0)
                        {
                            int i = 3*ispin;
                            int j = 3*jspin;

                            hessian(i+2, j+1) +=  dmi_magnitudes[i_pair] * dmi_normals[i_pair][0];
                            hessian(i+1, j+2) += -dmi_magnitudes[i_pair] * dmi_normals[i_pair][0];
                            hessian(i, j+2)   +=  dmi_magnitudes[i_pair] * dmi_normals[i_pair][1];
                            hessian(i+2, j)   += -dmi_magnitudes[i_pair] * dmi_normals[i_pair][1];
                            hessian(i+1, j)   +=  dmi_magnitudes[i_pair] * dmi_normals[i_pair][2];
                            hessian(i, j+1)   += -dmi_magnitudes[i_pair] * dmi_normals[i_pair][2];

                            #ifndef _OPENMP
                            hessian(j+1, i+2) +=  dmi_magnitudes[i_pair] * dmi_normals[i_pair][0];
                            hessian(j+2, i+1) += -dmi_magnitudes[i_pair] * dmi_normals[i_pair][0];
                            hessian(j+2, i)   +=  dmi_magnitudes[i_pair] * dmi_normals[i_pair][1];
                            hessian(j, i+2)   += -dmi_magnitudes[i_pair] * dmi_normals[i_pair][1];
                            hessian(j, i+1)   +=  dmi_magnitudes[i_pair] * dmi_normals[i_pair][2];
                            hessian(j+1, i)   += -dmi_magnitudes[i_pair] * dmi_normals[i_pair][2];
                            #endif
                        }
                    }
                }
            }
        }

        // Tentative Dipole-Dipole (Note: this is very tentative and could be wrong)
        field<int> tupel1 = field<int>(4);
        field<int> tupel2 = field<int>(4);
        field<int> maxVal = {geometry->n_cell_atoms, geometry->n_cells[0], geometry->n_cells[1], geometry->n_cells[2]};

        if(save_dipole_matrices && ddi_method == DDI_Method::FFT && false)
        {
            for(int idx1 = 0; idx1 < geometry->nos; idx1++)
            {
                Engine::Vectormath::tupel_from_idx(idx1, tupel1, maxVal); //tupel1 now is {ib1, a1, b1, c1}
                for(int idx2 = 0; idx2 < geometry->nos; idx2++)
                {
                    Engine::Vectormath::tupel_from_idx(idx2, tupel2, maxVal); //tupel2 now is {ib2, a2, b2, c2}
                    int& b_inter = inter_sublattice_lookup[tupel1[0] + geometry->n_cell_atoms * tupel2[0]];
                    int da = tupel2[1] - tupel1[1];
                    int db = tupel2[2] - tupel1[2];
                    int dc = tupel2[3] - tupel1[3];
                    Matrix3 & D = dipole_matrices[b_inter + n_inter_sublattice * (da + geometry->n_cells[0] * (db + geometry->n_cells[1] * dc))];
                
                    int i = 3 * idx1;
                    int j = 3 * idx2;

                    for(int alpha1 = 0; alpha1 < 3; alpha1++)
                        for(int alpha2 = 0; alpha2 < 3; alpha2++)
                        {
                            hessian(i + alpha1, j + alpha2) = D(alpha1, alpha2);
                            hessian(j + alpha1, i + alpha2) = D(alpha1, alpha2);
                        }
                }
            }
        }
                 
        //// Dipole-Dipole
        //for (unsigned int i_pair = 0; i_pair < this->DD_indices.size(); ++i_pair)
        //{
        //	// indices
        //	int idx_1 = DD_indices[i_pair][0];
        //	int idx_2 = DD_indices[i_pair][1];
        //	// prefactor
        //	scalar prefactor = 0.0536814951168
        //		* mu_s[idx_1] * mu_s[idx_2]
        //		/ std::pow(DD_magnitude[i_pair], 3);
        //	// components
        //	for (int alpha = 0; alpha < 3; ++alpha)
        //	{
        //		for (int beta = 0; beta < 3; ++beta)
        //		{
        //			int idx_h = idx_1 + alpha*nos + 3 * nos*(idx_2 + beta*nos);
        //			if (alpha == beta)
        //				hessian[idx_h] += prefactor;
        //			hessian[idx_h] += -3.0*prefactor*DD_normal[i_pair][alpha] * DD_normal[i_pair][beta];
        //		}
        //	}
        //}

        // Quadruplets
    }

    void Hamiltonian_Heisenberg::FFT_Spins(const vectorfield & spins)
    {
        //size of original geometry
        int Na = geometry->n_cells[0];
        int Nb = geometry->n_cells[1];
        int Nc = geometry->n_cells[2];
        //bravais vectors
        Vector3 ta = geometry->bravais_vectors[0];
        Vector3 tb = geometry->bravais_vectors[1];
        Vector3 tc = geometry->bravais_vectors[2];
        int B = geometry->n_cell_atoms;

        auto& fft_spin_inputs = fft_plan_spins.real_ptr;
       
            //iterate over the **original** system
        #pragma omp parallel for collapse(4)
        for(int c = 0; c < Nc; ++c)
        {
            for(int b = 0; b < Nb; ++b)
            {
                for(int a = 0; a < Na; ++a)
                {
                    for(int bi = 0; bi < B; ++bi)
                    {
                        int idx_orig = bi + B * (a + Na * (b + Nb * c));
                        int idx = bi * spin_stride.basis + a * spin_stride.a + b * spin_stride.b + c * spin_stride.c;

                        fft_spin_inputs[idx                        ] = spins[idx_orig][0] * geometry->mu_s[idx_orig];
                        fft_spin_inputs[idx + 1 * spin_stride.comp ] = spins[idx_orig][1] * geometry->mu_s[idx_orig];
                        fft_spin_inputs[idx + 2 * spin_stride.comp ] = spins[idx_orig][2] * geometry->mu_s[idx_orig];
                    }
                }
            }
        }//end iteration over basis
        FFT::batch_Four_3D(fft_plan_spins);
    }

    void Hamiltonian_Heisenberg::FFT_Dipole_Matrices(int img_a, int img_b, int img_c)
    {
        //prefactor of ddi interaction
        scalar mult = C::mu_0 * C::mu_B * C::mu_B / ( 4*C::Pi * 1e-30 );

        //size of original geometry
        int Na = geometry->n_cells[0];
        int Nb = geometry->n_cells[1];
        int Nc = geometry->n_cells[2];
        //bravais vectors
        Vector3 ta = geometry->bravais_vectors[0];
        Vector3 tb = geometry->bravais_vectors[1];
        Vector3 tc = geometry->bravais_vectors[2];

        auto& fft_dipole_inputs = fft_plan_dipole.real_ptr;

        int b_inter = -1;
        for(int i_b1 = 0; i_b1 < geometry->n_cell_atoms; ++i_b1)
        {
            for(int i_b2 = 0; i_b2 < geometry->n_cell_atoms; ++i_b2)
            {
                if(i_b1 == i_b2 && i_b1 !=0)
                {
                    inter_sublattice_lookup[i_b1 + i_b2 * geometry->n_cell_atoms] = 0;
                    continue;
                }
                b_inter++;
                inter_sublattice_lookup[i_b1 + i_b2 * geometry->n_cell_atoms] = b_inter;

                //iterate over the padded system
                #pragma omp parallel for collapse(3)
                for(int c = 0; c < n_cells_padded[2]; ++c)
                {
                    for(int b = 0; b < n_cells_padded[1]; ++b)
                    {
                        for(int a = 0; a < n_cells_padded[0]; ++a)
                        {
                            int a_idx = a < Na ? a : a - n_cells_padded[0];
                            int b_idx = b < Nb ? b : b - n_cells_padded[1];
                            int c_idx = c < Nc ? c : c - n_cells_padded[2];
                            scalar Dxx = 0, Dxy = 0, Dxz = 0, Dyy = 0, Dyz = 0, Dzz = 0;
                            Vector3 diff;
                            //iterate over periodic images

                            for(int a_pb = - img_a; a_pb <= img_a; a_pb++)
                            {
                                for(int b_pb = - img_b; b_pb <= img_b; b_pb++)
                                {
                                    for(int c_pb = -img_c; c_pb <= img_c; c_pb++)
                                    {
                                        diff =    geometry->lattice_constant * (a_idx + a_pb * Na + geometry->cell_atoms[i_b1][0] - geometry->cell_atoms[i_b2][0]) * ta
                                                + geometry->lattice_constant * (b_idx + b_pb * Nb + geometry->cell_atoms[i_b1][1] - geometry->cell_atoms[i_b2][1]) * tb
                                                + geometry->lattice_constant * (c_idx + c_pb * Nc + geometry->cell_atoms[i_b1][2] - geometry->cell_atoms[i_b2][2]) * tc;
                                        if(diff.norm() > 1e-10)
                                        {
                                            auto d = diff.norm();
                                            auto d3 = d * d * d;
                                            auto d5 = d * d * d * d * d;
                                            Dxx += mult * (3 * diff[0]*diff[0] / d5 - 1/d3);
                                            Dxy += mult *  3 * diff[0]*diff[1] / d5;          //same as Dyx
                                            Dxz += mult *  3 * diff[0]*diff[2] / d5;          //same as Dzx
                                            Dyy += mult * (3 * diff[1]*diff[1] / d5 - 1/d3);
                                            Dyz += mult *  3 * diff[1]*diff[2] / d5;          //same as Dzy
                                            Dzz += mult * (3 * diff[2]*diff[2] / d5 - 1/d3);
                                        }
                                    }
                                }
                            }

                            int idx = b_inter * dipole_stride.basis + a * dipole_stride.a + b * dipole_stride.b + c * dipole_stride.c;

                            fft_dipole_inputs[idx                    ] = Dxx;
                            fft_dipole_inputs[idx + 1 * dipole_stride.comp] = Dxy;
                            fft_dipole_inputs[idx + 2 * dipole_stride.comp] = Dxz;
                            fft_dipole_inputs[idx + 3 * dipole_stride.comp] = Dyy;
                            fft_dipole_inputs[idx + 4 * dipole_stride.comp] = Dyz;
                            fft_dipole_inputs[idx + 5 * dipole_stride.comp] = Dzz;

                            //We explicitly ignore the different strides etc. here
                            if(save_dipole_matrices && a < Na && b < Nb && c < Nc)
                            {
                                dipole_matrices[b_inter + n_inter_sublattice * (a + Na * (b + Nb * c))] <<    Dxx, Dxy, Dxz,
                                                                                                        Dxy, Dyy, Dyz,
                                                                                                        Dxz, Dyz, Dzz;
                            }
                        }
                    }
                }
            }
        }
        FFT::batch_Four_3D(fft_plan_dipole);
    }

    void Hamiltonian_Heisenberg::Prepare_DDI()
    {
        n_cells_padded.resize(3);
        n_cells_padded[0] = (geometry->n_cells[0] > 1) ? 2 * geometry->n_cells[0] : 1;
        n_cells_padded[1] = (geometry->n_cells[1] > 1) ? 2 * geometry->n_cells[1] : 1;
        n_cells_padded[2] = (geometry->n_cells[2] > 1) ? 2 * geometry->n_cells[2] : 1;

        FFT::FFT_Init();

        //workaround for bug in kissfft 
        //kissfft_ndr does not perform one-dimensional ffts properly
        #ifndef SPIRIT_USE_FFTW
        int number_of_one_dims = 0;
        for(int i=0; i<3; i++)
            if(n_cells_padded[i] == 1 && ++number_of_one_dims > 1)
                n_cells_padded[i] = 2;
        #endif

        sublattice_size = n_cells_padded[0] * n_cells_padded[1] * n_cells_padded[2];

        inter_sublattice_lookup.resize(geometry->n_cell_atoms * geometry->n_cell_atoms);


        //we dont need to transform over length 1 dims
        std::vector<int> fft_dims;
        for(int i = 2; i >= 0; i--) //notice that reverse order is important!
        {
            if(n_cells_padded[i] > 1)
                fft_dims.push_back(n_cells_padded[i]);
        }
        

        //Count how many distinct inter-lattice contributions we need to store
        n_inter_sublattice = 0;
        for(int i = 0; i < geometry->n_cell_atoms; i++)
        {
            for(int j = 0; j < geometry->n_cell_atoms; j++)
            {
                if(i != 0 && i==j) continue;
                n_inter_sublattice++;
            }
        }

        //Create fft plans.
        fft_plan_dipole.dims     = fft_dims;
        fft_plan_dipole.inverse  = false;
        fft_plan_dipole.howmany  = 6 * n_inter_sublattice;
        fft_plan_dipole.real_ptr = field<FFT::FFT_real_type>(n_inter_sublattice * 6 * sublattice_size);
        fft_plan_dipole.cpx_ptr  = field<FFT::FFT_cpx_type>(n_inter_sublattice * 6 * sublattice_size);
        fft_plan_dipole.Create_Configuration();

        fft_plan_spins.dims     = fft_dims;
        fft_plan_spins.inverse  = false;
        fft_plan_spins.howmany  = 3 * geometry->n_cell_atoms;
        fft_plan_spins.real_ptr = field<FFT::FFT_real_type>(3 * sublattice_size * geometry->n_cell_atoms);
        fft_plan_spins.cpx_ptr  = field<FFT::FFT_cpx_type>(3 * sublattice_size * geometry->n_cell_atoms);
        fft_plan_spins.Create_Configuration();

        fft_plan_reverse.dims     = fft_dims;
        fft_plan_reverse.inverse  = true;
        fft_plan_reverse.howmany  = 3 * geometry->n_cell_atoms;
        fft_plan_reverse.cpx_ptr  = field<FFT::FFT_cpx_type>(3 * sublattice_size * geometry->n_cell_atoms);
        fft_plan_reverse.real_ptr = field<FFT::FFT_real_type>(3 * sublattice_size * geometry->n_cell_atoms);
        fft_plan_reverse.Create_Configuration();

        #ifdef SPIRIT_USE_FFTW
            field<int*> temp_s = {&spin_stride.comp, &spin_stride.basis, &spin_stride.a, &spin_stride.b, &spin_stride.c};
            field<int*> temp_d = {&dipole_stride.comp, &dipole_stride.basis, &dipole_stride.a, &dipole_stride.b, &dipole_stride.c};;
            FFT::get_strides(temp_s, {3, this->geometry->n_cell_atoms, n_cells_padded[0], n_cells_padded[1], n_cells_padded[2]});
            FFT::get_strides(temp_d, {6, n_inter_sublattice, n_cells_padded[0], n_cells_padded[1], n_cells_padded[2]});
            it_bounds_pointwise_mult  = {   (n_cells_padded[0]/2 + 1), // due to redundancy in real fft
                                            n_cells_padded[1], 
                                            n_cells_padded[2] 
                                        };
        #else
            field<int*> temp_s = {&spin_stride.a, &spin_stride.b, &spin_stride.c, &spin_stride.comp, &spin_stride.basis};
            field<int*> temp_d = {&dipole_stride.a, &dipole_stride.b, &dipole_stride.c, &dipole_stride.comp, &dipole_stride.basis};;
            FFT::get_strides(temp_s, {n_cells_padded[0], n_cells_padded[1], n_cells_padded[2], 3, this->geometry->n_cell_atoms});
            FFT::get_strides(temp_d, {n_cells_padded[0], n_cells_padded[1], n_cells_padded[2], 6, n_inter_sublattice});
            it_bounds_pointwise_mult  = {   n_cells_padded[0], 
                                            n_cells_padded[1], 
                                            n_cells_padded[2] 
                                        };
            (it_bounds_pointwise_mult[fft_dims.size() - 1] /= 2 )++;
        #endif

        //perform FFT of dipole matrices
        int img_a = boundary_conditions[0] == 0 ? 0 : ddi_n_periodic_images[0];
        int img_b = boundary_conditions[1] == 0 ? 0 : ddi_n_periodic_images[1];
        int img_c = boundary_conditions[2] == 0 ? 0 : ddi_n_periodic_images[2];

        if(save_dipole_matrices)
            dipole_matrices = field<Matrix3>(n_inter_sublattice * geometry->n_cells_total);
        FFT_Dipole_Matrices(img_a, img_b, img_c);
        fft_plan_dipole.real_ptr = field<FFT::FFT_real_type>();
        fft_plan_dipole.Free_Configuration();
    }

    void Hamiltonian_Heisenberg::Clean_DDI()
    {
        fft_plan_spins.Clean();
        fft_plan_dipole.Clean();
        fft_plan_reverse.Clean();
    }

    // Hamiltonian name as string
    static const std::string name = "Heisenberg";
    const std::string& Hamiltonian_Heisenberg::Name() { return name; }
}

#endif
